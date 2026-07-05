// jarvis/src/stt/moonshine_node.cpp — Moonshine-tiny(-ko) ONNX STT 구현.
//
// 파이프라인 (moonshine_ref.py 골든과 1:1 — optimum 표준구현과 일치 검증됨):
//   encoder(input_values 16k raw) → last_hidden_state [1, T, 288]
//   decoder(input_ids=[BOS], encoder_hidden_states) → logits + present KV(dec+enc)
//   decoder_with_past(input_ids=[tok], past dec+enc KV) → logits + present dec KV
//     (enc KV 는 첫 패스에서 얻어 매 스텝 상수 재사용)
//   argmax 반복 → EOS(2) 또는 max_length. tokenizer decode → user_text.

#include "moonshine_node.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace jarvis::stt {

namespace {
constexpr int   N_LAYERS = 6;
constexpr int   N_HEADS  = 8;
constexpr int   HEAD_DIM = 36;
constexpr int64_t BOS = 1, EOS = 2;

// PCM 을 16kHz 로 선형 리샘플 (이미 16k 면 그대로)
std::vector<float> resample_to_16k(const std::vector<float>& in, int sr) {
    if (sr == 16000 || in.empty() || sr <= 0) return in;
    const double ratio = 16000.0 / sr;
    const std::size_t n = static_cast<std::size_t>(in.size() * ratio);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double src = i / ratio;
        const std::size_t j = static_cast<std::size_t>(src);
        const double frac = src - j;
        out[i] = (j + 1 < in.size())
                   ? static_cast<float>(in[j] * (1.0 - frac) + in[j + 1] * frac)
                   : in[j];
    }
    return out;
}
}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 생성자 — ONNX 세션 3개 + 토크나이저 vocab 로드
// ─────────────────────────────────────────────────────────────────────────────
MoonshineSttNode::MoonshineSttNode(std::string name, const neograph::json& cfg)
    : name_(std::move(name)) {
    language_   = cfg.value("language", std::string("ko"));
    max_length_ = cfg.value("max_length", 194);

    const std::string dir = cfg.value("model_dir",
                                      std::string("assets/moonshine-tiny-ko"));
    auto path = [&](const char* key, const char* dflt) {
        return dir + "/" + cfg.value(key, std::string(dflt));
    };

    // ── 토크나이저 (tokenizer.json 의 model.vocab → id_to_piece_) ──
    try {
        std::ifstream tf(path("tokenizer", "tokenizer.json"));
        if (tf) {
            std::ostringstream ss; ss << tf.rdbuf();
            auto tj = neograph::json::parse(ss.str());
            const auto& vocab = tj["model"]["vocab"];
            id_to_piece_.resize(vocab.size());
            for (auto it = vocab.begin(); it != vocab.end(); ++it) {
                const int id = it.value().get<int>();
                if (id >= 0 && id < static_cast<int>(id_to_piece_.size()))
                    id_to_piece_[id] = it.key();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[moonshine] tokenizer 로드 실패: " << e.what() << "\n";
    }

#ifdef JARVIS_HAVE_ONNXRUNTIME
    try {
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "jarvis_moonshine");
        mem_ = std::make_unique<Ort::MemoryInfo>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(std::max(1u, std::thread::hardware_concurrency() / 2));
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

        enc_  = std::make_unique<Ort::Session>(*env_,
                    path("encoder", "onnx/encoder_model_int8.onnx").c_str(), so);
        dec0_ = std::make_unique<Ort::Session>(*env_,
                    path("decoder", "onnx/decoder_model_int8.onnx").c_str(), so);
        decp_ = std::make_unique<Ort::Session>(*env_,
                    path("decoder_with_past", "onnx/decoder_with_past_model_int8.onnx").c_str(), so);

        Ort::AllocatorWithDefaultOptions alloc;
        auto names = [&](Ort::Session& s, bool inputs) {
            std::vector<std::string> v;
            const size_t n = inputs ? s.GetInputCount() : s.GetOutputCount();
            for (size_t i = 0; i < n; ++i) {
                auto nm = inputs ? s.GetInputNameAllocated(i, alloc)
                                 : s.GetOutputNameAllocated(i, alloc);
                v.emplace_back(nm.get());
            }
            return v;
        };
        enc_in_  = names(*enc_, true);   enc_out_  = names(*enc_, false);
        dec0_in_ = names(*dec0_, true);  dec0_out_ = names(*dec0_, false);
        decp_in_ = names(*decp_, true);  decp_out_ = names(*decp_, false);

        ready_ = !id_to_piece_.empty();
        if (ready_)
            std::cerr << "[moonshine] 모델 로드 완료 (" << dir
                      << ", lang=" << language_ << ", vocab=" << id_to_piece_.size() << ")\n";
    } catch (const std::exception& e) {
        std::cerr << "[moonshine] ONNX 세션 로드 실패 — mock 폴백: " << e.what() << "\n";
        ready_ = false;
    }
#endif
}

MoonshineSttNode::~MoonshineSttNode() = default;

// ─────────────────────────────────────────────────────────────────────────────
// 토크나이저 디코드 — HF decoder Sequence:
//   Replace(▁→space) → ByteFallback(<0xHH>→byte) → Fuse(byte 연결) → Strip(앞 1)
// ─────────────────────────────────────────────────────────────────────────────
std::string MoonshineSttNode::decode_tokens(const std::vector<int64_t>& ids) const {
    static const std::string kMeta = "\xe2\x96\x81";  // ▁ U+2581
    std::string out;
    for (int64_t id : ids) {
        if (id < 0 || id >= static_cast<int64_t>(id_to_piece_.size())) continue;
        const std::string& p = id_to_piece_[id];
        // ByteFallback: "<0xHH>" → raw byte
        if (p.size() == 6 && p[0] == '<' && p[1] == '0' && p[2] == 'x' && p[5] == '>') {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return 0;
            };
            out.push_back(static_cast<char>(hex(p[3]) * 16 + hex(p[4])));
            continue;
        }
        // Replace ▁ → space (나머지는 그대로 — 이미 UTF-8)
        std::size_t pos = 0, prev = 0;
        while ((pos = p.find(kMeta, prev)) != std::string::npos) {
            out.append(p, prev, pos - prev);
            out.push_back(' ');
            prev = pos + kMeta.size();
        }
        out.append(p, prev, std::string::npos);
    }
    // Strip 앞쪽 공백 1개
    if (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out;
}

#ifdef JARVIS_HAVE_ONNXRUNTIME
namespace {
// 소유 버퍼 기반 KV 텐서 — Ort::Value 는 버퍼를 복사 안 하므로 buffer 를 소유해
// Run 수명 동안 살려둔다.
struct Tensor {
    std::vector<float>   data;
    std::vector<int64_t> shape;
};
Ort::Value make_f32(Ort::MemoryInfo& mem, Tensor& t) {
    return Ort::Value::CreateTensor<float>(mem, t.data.data(), t.data.size(),
                                           t.shape.data(), t.shape.size());
}
int64_t argmax_last_row(const float* logits, int64_t seq, int64_t vocab) {
    const float* row = logits + (seq - 1) * vocab;
    int64_t best = 0; float bv = row[0];
    for (int64_t v = 1; v < vocab; ++v) if (row[v] > bv) { bv = row[v]; best = v; }
    return best;
}
}  // namespace
#endif

// ─────────────────────────────────────────────────────────────────────────────
// run()
// ─────────────────────────────────────────────────────────────────────────────
asio::awaitable<neograph::graph::NodeOutput>
MoonshineSttNode::run(neograph::graph::NodeInput in) {
    neograph::graph::NodeOutput out;

    // voice_in 에서 PCM + sample_rate 추출 (텍스트 모드면 mock_text 패스)
    const auto& vp = in.state.get("voice_in");
    std::vector<float> pcm;
    int sr = 0;
    std::string mock_text;
    if (vp.is_object()) {
        if (vp.contains("mock_text") && vp["mock_text"].is_string())
            mock_text = vp["mock_text"].get<std::string>();
        if (vp.contains("sample_rate") && vp["sample_rate"].is_number())
            sr = vp["sample_rate"].get<int>();
        if (vp.contains("pcm") && vp["pcm"].is_array()) {
            pcm.reserve(vp["pcm"].size());
            for (const auto& s : vp["pcm"]) pcm.push_back(s.get<float>());
        }
    }

    // 텍스트 모드(mock) 또는 미준비 → mock_text 패스스루 (whisper_node 와 동일 계약)
    if (!ready_ || pcm.empty()) {
        out.writes.push_back({"user_text", mock_text});
        out.writes.push_back({"user_lang",
            mock_text.empty() ? std::string("en") : language_});
        co_return out;
    }

#ifdef JARVIS_HAVE_ONNXRUNTIME
    try {
        pcm = resample_to_16k(pcm, sr ? sr : 16000);

        // ── 인코더 ──
        std::array<int64_t, 2> iv_shape{1, static_cast<int64_t>(pcm.size())};
        Ort::Value iv = Ort::Value::CreateTensor<float>(
            *mem_, pcm.data(), pcm.size(), iv_shape.data(), iv_shape.size());
        std::vector<const char*> enc_in{enc_in_[0].c_str()};
        std::vector<const char*> enc_out{enc_out_[0].c_str()};
        auto enc_r = enc_->Run(Ort::RunOptions{nullptr}, enc_in.data(), &iv, 1,
                               enc_out.data(), 1);
        auto enc_shape = enc_r[0].GetTensorTypeAndShapeInfo().GetShape();  // [1,T,288]
        const float* enc_ptr = enc_r[0].GetTensorData<float>();
        const int64_t enc_seq = enc_shape[1], enc_hid = enc_shape[2];
        std::vector<float> enc_hidden(enc_ptr, enc_ptr + (enc_seq * enc_hid));

        // ── 첫 패스: decoder_model ──
        int64_t cur = BOS;
        std::array<int64_t, 2> id_shape{1, 1};
        std::vector<Ort::Value> d0_vals;
        d0_vals.emplace_back(Ort::Value::CreateTensor<int64_t>(
            *mem_, &cur, 1, id_shape.data(), id_shape.size()));
        std::array<int64_t, 3> eh_shape{1, enc_seq, enc_hid};
        d0_vals.emplace_back(Ort::Value::CreateTensor<float>(
            *mem_, enc_hidden.data(), enc_hidden.size(),
            eh_shape.data(), eh_shape.size()));
        std::vector<const char*> d0_in{dec0_in_[0].c_str(), dec0_in_[1].c_str()};
        std::vector<const char*> d0_out;
        for (auto& n : dec0_out_) d0_out.push_back(n.c_str());
        auto d0 = dec0_->Run(Ort::RunOptions{nullptr}, d0_in.data(),
                             d0_vals.data(), d0_vals.size(),
                             d0_out.data(), d0_out.size());

        auto out_index = [](const std::vector<std::string>& names, const std::string& key) {
            for (size_t i = 0; i < names.size(); ++i) if (names[i] == key) return i;
            return static_cast<size_t>(0);
        };
        auto vocab = d0[0].GetTensorTypeAndShapeInfo().GetShape().back();
        auto d0_logits_shape = d0[0].GetTensorTypeAndShapeInfo().GetShape();
        cur = argmax_last_row(d0[0].GetTensorData<float>(), d0_logits_shape[1], vocab);

        // present KV → 소유 버퍼 (dec: 갱신, enc: 상수)
        auto grab = [&](std::vector<Ort::Value>& r, const std::vector<std::string>& names,
                        const std::string& key) -> Tensor {
            size_t idx = out_index(names, key);
            auto sh = r[idx].GetTensorTypeAndShapeInfo().GetShape();
            const float* p = r[idx].GetTensorData<float>();
            int64_t cnt = 1; for (auto d : sh) cnt *= d;
            return Tensor{std::vector<float>(p, p + cnt), sh};
        };
        std::vector<Tensor> dec_kv(N_LAYERS * 2), enc_kv(N_LAYERS * 2);
        for (int L = 0; L < N_LAYERS; ++L) {
            dec_kv[L*2+0] = grab(d0, dec0_out_, "present." + std::to_string(L) + ".decoder.key");
            dec_kv[L*2+1] = grab(d0, dec0_out_, "present." + std::to_string(L) + ".decoder.value");
            enc_kv[L*2+0] = grab(d0, dec0_out_, "present." + std::to_string(L) + ".encoder.key");
            enc_kv[L*2+1] = grab(d0, dec0_out_, "present." + std::to_string(L) + ".encoder.value");
        }

        std::vector<int64_t> tokens;
        if (cur != EOS) tokens.push_back(cur);

        // ── 반복: decoder_with_past ──
        std::vector<const char*> dp_in;  for (auto& n : decp_in_)  dp_in.push_back(n.c_str());
        std::vector<const char*> dp_out; for (auto& n : decp_out_) dp_out.push_back(n.c_str());
        for (int step = 1; step < max_length_ && cur != EOS; ++step) {
            // 입력 조립: decp_in_ 순서대로 (input_ids, past.L.dec/enc.k/v ...)
            std::vector<Ort::Value> vals;
            int64_t idbuf = cur;
            for (const auto& nm : decp_in_) {
                if (nm == "input_ids") {
                    vals.emplace_back(Ort::Value::CreateTensor<int64_t>(
                        *mem_, &idbuf, 1, id_shape.data(), id_shape.size()));
                } else {
                    // past_key_values.L.{decoder|encoder}.{key|value}
                    int L = std::stoi(nm.substr(16, nm.find('.', 16) - 16));
                    bool is_enc = nm.find(".encoder.") != std::string::npos;
                    bool is_val = nm.substr(nm.size() - 5) == "value";
                    Tensor& t = (is_enc ? enc_kv : dec_kv)[L*2 + (is_val ? 1 : 0)];
                    vals.emplace_back(make_f32(*mem_, t));
                }
            }
            auto dp = decp_->Run(Ort::RunOptions{nullptr}, dp_in.data(),
                                 vals.data(), vals.size(), dp_out.data(), dp_out.size());
            auto ls = dp[0].GetTensorTypeAndShapeInfo().GetShape();
            cur = argmax_last_row(dp[0].GetTensorData<float>(), ls[1], ls.back());
            if (cur == EOS) break;
            tokens.push_back(cur);
            // dec KV 갱신 (enc KV 는 그대로)
            std::vector<Tensor> nd(N_LAYERS * 2);
            for (int L = 0; L < N_LAYERS; ++L) {
                nd[L*2+0] = grab(dp, decp_out_, "present." + std::to_string(L) + ".decoder.key");
                nd[L*2+1] = grab(dp, decp_out_, "present." + std::to_string(L) + ".decoder.value");
            }
            dec_kv = std::move(nd);
        }

        std::string text = decode_tokens(tokens);
        std::cerr << "[moonshine] " << tokens.size() << " tokens → " << text << "\n";
        out.writes.push_back({"user_text", text});
        out.writes.push_back({"user_lang", language_});
        co_return out;
    } catch (const std::exception& e) {
        std::cerr << "[moonshine] 추론 오류 — 빈 결과: " << e.what() << "\n";
    }
#endif

    out.writes.push_back({"user_text", mock_text});
    out.writes.push_back({"user_lang", language_});
    co_return out;
}

}  // namespace jarvis::stt
