// jarvis/src/stt/whisper_node.cpp
//
// WhisperSttNode 구현.
//
// JARVIS_HAVE_WHISPER=1 이면 진짜 whisper.cpp 호출,
// 없으면 mock(텍스트 그대로 패스스루 + 한글 코드포인트 휴리스틱) 으로 동작.
//
// 두 가지 입력 모드 (voice_in 채널):
//   텍스트 모드: {"mock_text": "...", "pcm": [], "sample_rate": 0}
//               — STT 건너뛰고 mock_text 를 user_text 에 그대로 씀
//   wav 모드   : {"mock_text": "", "pcm": [float...], "sample_rate": 16000}
//               — whisper.cpp 로 실제 음성 인식 수행
//
// 헤더(whisper_node.h)를 수정하지 않으므로 whisper_context* 는
// 파일 정적 맵으로 노드 인스턴스와 연결한다.

#include "whisper_node.h"

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// whisper.cpp 연동 — JARVIS_HAVE_WHISPER 가드
// ─────────────────────────────────────────────────────────────────────────────

#ifdef JARVIS_HAVE_WHISPER
#  include <whisper.h>
#endif

namespace jarvis::stt {

// ─────────────────────────────────────────────────────────────────────────────
// 파일 정적 맵: 노드 인스턴스 → whisper_context*
//
// 헤더를 수정하지 않고 whisper_context* 를 보관하는 유일한 방법.
// 노드가 소멸할 때 해당 항목을 지워야 한다.
// ─────────────────────────────────────────────────────────────────────────────

#ifdef JARVIS_HAVE_WHISPER
namespace {
std::unordered_map<WhisperSttNode*, whisper_context*> g_ctx_map;
}  // namespace
#endif

// ─────────────────────────────────────────────────────────────────────────────
// 생성자 — cfg 설정값 저장 + 모델 1회 로드
// ─────────────────────────────────────────────────────────────────────────────

WhisperSttNode::WhisperSttNode(std::string name, const neograph::json& cfg)
    : name_(std::move(name))
    , model_path_(cfg.value("model_path", std::string("assets/whisper-small.bin")))
    , language_(cfg.value("language",     std::string("auto")))
    , translate_to_en_(cfg.value("translate_to_en", false))
{
#ifdef JARVIS_HAVE_WHISPER
    // 모델 로드 (1회) — 무거운 작업이므로 생성자에서 미리 처리
    whisper_context_params cparams = whisper_context_default_params();
    whisper_context* ctx = whisper_init_from_file_with_params(model_path_.c_str(), cparams);
    if (ctx) {
        g_ctx_map[this] = ctx;
        std::cout << "[whisper] 모델 로드 완료: " << model_path_ << "\n";
    } else {
        std::cerr << "[whisper] 모델 로드 실패: " << model_path_
                  << " — mock 모드로 동작\n";
        // ctx == nullptr → g_ctx_map 에 등록 안 함 → run() 에서 mock 경로로 처리
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// 소멸자 — whisper_context 해제 + 맵 정리
// ─────────────────────────────────────────────────────────────────────────────

WhisperSttNode::~WhisperSttNode() {
#ifdef JARVIS_HAVE_WHISPER
    auto it = g_ctx_map.find(this);
    if (it != g_ctx_map.end()) {
        whisper_free(it->second);
        g_ctx_map.erase(it);
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// 언어 감지 휴리스틱 (mock 전용) — 한글 코드포인트(가-힣, U+AC00–U+D7A3) 있으면 "ko"
// whisper 가 있을 때는 whisper_full_lang_id() 가 대신함.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::string detect_language_heuristic(const std::string& text) {
    if (text.empty()) return "en";

    // UTF-8 한글 범위: 0xEA 0xB0 0x80 (가) ~ 0xED 0x9E 0xA3 (힣)
    // 3바이트 시퀀스. 첫 바이트가 0xEA~0xED 인 케이스를 빠르게 걸러냄.
    for (size_t i = 0; i + 2 < text.size(); ) {
        unsigned char b0 = static_cast<unsigned char>(text[i]);
        if (b0 >= 0xEA && b0 <= 0xED) {
            unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
            unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
            // 가 = EA B0 80, 힣 = ED 9E A3
            uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
            if (cp >= 0xAC00 && cp <= 0xD7A3) {
                return "ko";
            }
            i += 3;
        } else if (b0 < 0x80) {
            // ASCII — 1바이트
            ++i;
        } else if (b0 < 0xC0) {
            // UTF-8 연속 바이트 — 다음 위치로
            ++i;
        } else if (b0 < 0xE0) {
            i += 2;
        } else if (b0 < 0xF0) {
            i += 3;
        } else {
            i += 4;
        }
    }
    return "en";
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// run() — voice_in 패킷 읽어 user_text + user_lang 채널에 쓰기
//
// 분기:
//   1) PCM 데이터 있음 + JARVIS_HAVE_WHISPER → 진짜 whisper 호출 (wav 모드)
//   2) PCM 없고 mock_text 있음               → 텍스트 그대로 패스 (텍스트 모드)
//   3) 둘 다 비어있음                         → 빈 user_text + "en" 패스 (EOF 호환)
// ─────────────────────────────────────────────────────────────────────────────

asio::awaitable<neograph::graph::NodeOutput>
WhisperSttNode::run(neograph::graph::NodeInput in)
{
    neograph::graph::NodeOutput out;

    // voice_in 채널에서 패킷 읽기
    const auto& voice_packet = in.state.get("voice_in");

    // ── PCM 데이터 추출 ────────────────────────────────────────────────────
    std::vector<float> pcm;
    int sample_rate = 0;

    if (voice_packet.is_object()) {
        if (voice_packet.contains("pcm") && voice_packet["pcm"].is_array()) {
            const auto& pcm_json = voice_packet["pcm"];
            pcm.reserve(pcm_json.size());
            for (const auto& v : pcm_json) {
                pcm.push_back(v.get<float>());
            }
        }
        if (voice_packet.contains("sample_rate") && voice_packet["sample_rate"].is_number()) {
            sample_rate = voice_packet["sample_rate"].get<int>();
        }
    }

    // ── 분기 처리 ──────────────────────────────────────────────────────────

    std::string text;
    std::string lang;

#ifdef JARVIS_HAVE_WHISPER
    auto ctx_it = g_ctx_map.find(this);
    whisper_context* ctx = (ctx_it != g_ctx_map.end()) ? ctx_it->second : nullptr;

    if (!pcm.empty() && ctx) {
        // ── wav 모드: 진짜 whisper.cpp 음성 인식 ──────────────────────────

        std::cout << "[whisper] STT 시작 (PCM " << pcm.size()
                  << " samples, lang=" << language_ << ")\n";

        // 음성 인식 파라미터 설정 — greedy 방식으로 빠르게
        whisper_full_params wparams =
            whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

        // 언어 설정: "auto" 면 자동 감지, 아니면 ISO 코드 고정.
        // ⚠️ detect_language=true 는 whisper.cpp 에서 "언어만 감지하고 전사
        //    없이 종료" 모드 — 이걸 켜면 user_lang 은 나오는데 user_text 가
        //    항상 빈 문자열이 된다(실측: jfk.wav 도 ''). auto 감지 + 전사를
        //    둘 다 하려면 language="auto" + detect_language=false.
        if (language_ == "auto") {
            wparams.language        = "auto";
            wparams.detect_language = false;
        } else {
            wparams.language        = language_.c_str();
            wparams.detect_language = false;
        }

        // 영어 번역 여부
        wparams.translate = translate_to_en_;

        // 진행 상황 콘솔 출력 끄기 — [whisper] 로그만 남긴다
        wparams.print_progress  = false;
        wparams.print_realtime  = false;
        wparams.print_timestamps = false;

        // 음성 인식 실행
        int ret = whisper_full(ctx, wparams, pcm.data(), static_cast<int>(pcm.size()));
        if (ret != 0) {
            std::cerr << "[whisper] whisper_full 실패 (code=" << ret << ") — 빈 결과 반환\n";
            // 실패 시 빈 text + "en" 으로 처리
            lang = "en";
        } else {
            // 모든 세그먼트(구간) 텍스트 이어 붙이기
            int n_seg = whisper_full_n_segments(ctx);
            for (int i = 0; i < n_seg; ++i) {
                const char* seg_text = whisper_full_get_segment_text(ctx, i);
                if (seg_text) {
                    text += seg_text;
                }
            }

            // 감지된 언어 코드 추출 — int id → ISO 문자열 ("ko", "en", ...)
            int lang_id = whisper_full_lang_id(ctx);
            const char* lang_str = (lang_id >= 0) ? whisper_lang_str(lang_id) : nullptr;
            lang = (lang_str && lang_str[0] != '\0') ? lang_str : "en";

            std::cout << "[whisper] 인식: \"" << text
                      << "\" (감지된 언어: " << lang << ")\n";
        }
    } else if (ctx == nullptr && !pcm.empty()) {
        // whisper 컨텍스트(context) 없는데 PCM 들어온 경우 — 모델 로드 실패 상황
        // mock 텍스트가 있으면 그걸 쓰고, 없으면 빈 결과
        if (voice_packet.is_object() && voice_packet.contains("mock_text")) {
            const auto& mt = voice_packet["mock_text"];
            if (mt.is_string()) text = mt.get<std::string>();
        }
        lang = detect_language_heuristic(text);
    } else {
        // PCM 없음 → 텍스트 모드 또는 EOF
        if (voice_packet.is_object() && voice_packet.contains("mock_text")) {
            const auto& mt = voice_packet["mock_text"];
            if (mt.is_string()) text = mt.get<std::string>();
        }
        lang = detect_language_heuristic(text);
    }
#else
    // ── JARVIS_HAVE_WHISPER 없을 때 mock 동작 ─────────────────────────────
    // mock_text 필드 꺼내기 — 텍스트 모드와 동일하게 동작
    if (voice_packet.is_object() && voice_packet.contains("mock_text")) {
        const auto& mt = voice_packet["mock_text"];
        if (mt.is_string()) {
            text = mt.get<std::string>();
        }
    }
    lang = detect_language_heuristic(text);
#endif

    // ── user_text + user_lang 채널에 쓰기 ─────────────────────────────────
    out.writes.push_back({"user_text", text});
    out.writes.push_back({"user_lang", lang});

    co_return out;
}

}  // namespace jarvis::stt
