"""Live end-to-end matrix of the typed provider path (tests/live/README.md).

Every family preset buffered and streamed, vision input, forced tool calls, media output and the known gaps of this
release line. Nothing runs unless NEOGRAPH_E2E_LIVE=1 and the provider's key variable are set. Each call uses a
cheap model and a small cap.
"""

import pytest

import neograph_engine as ng

from live_support import ROUTES, TEXT_MODELS, Run, case, provider, red_png, user, weather_tool

MODES = (("collect", False), ("stream", True))


def _key(preset):
    return ROUTES[preset][4]


TEXT = [case(preset, stream, keys=[_key(preset)], id=f"{preset}-{name}")
        for preset in ROUTES for name, stream in MODES]


@pytest.mark.parametrize("preset, stream", TEXT)
def test_text(preset, stream):
    model, values = TEXT_MODELS[preset]
    run = Run(provider(preset, model), model, [user("Reply with one short word.")], stream=stream, **values)
    assert run.text().strip()
    usage = run.ok().usage
    assert usage.output_total is not None and usage.output_total.value > 0
    if stream:
        assert run.streamed_text() == run.text()


CLAUDE_55 = [case(model, stream, keys=["ANTHROPIC_API_KEY"], id=f"{model}-{name}")
             for model in ("claude-haiku-5-5", "claude-sonnet-5-5") for name, stream in MODES]


@pytest.mark.parametrize("model, stream", CLAUDE_55)
def test_claude_5_5_text(model, stream):
    run = Run(provider("anthropic.messages", model), model, [user("Reply with one short word.")], stream=stream,
              max_output_tokens=64)
    assert run.text().strip()
    if stream:
        assert run.streamed_text() == run.text()


VISION = [case(preset, keys=[_key(preset)], id=preset)
          for preset in ("openai.chat", "anthropic.messages", "google.generate")]


@pytest.mark.parametrize("preset", VISION)
def test_vision_input(preset):
    model, values = TEXT_MODELS[preset]
    message = user("What color is this image? Answer with one word.", ng.ProviderMedia.image("image/png", red_png()))
    run = Run(provider(preset, model), model, [message], **values)
    assert "red" in run.text().lower()


TOOLS = [case(preset, keys=[_key(preset)], id=preset) for preset in ROUTES]


@pytest.mark.parametrize("preset", TOOLS)
def test_tool_call(preset):
    model, values = TEXT_MODELS[preset]
    if preset == "openai.chat":
        # OpenAI refuses function tools on Chat Completions while the model reasons (HTTP 400, "set reasoning_effort
        # to 'none'"), so this case turns the reasoning off.
        values = {**values, "reasoning_effort": "none"}
    run = Run(provider(preset, model), model,
              [user("What is the weather in Seoul? Call get_weather with the city name.")],
              tools=[weather_tool()], **values)
    calls = [p for p in run.parts() if isinstance(p, ng.ProviderToolCall)]
    assert calls and calls[0].name == "get_weather"


def _images(run):
    return [p for p in run.parts() if isinstance(p, ng.ProviderMedia) and p.kind == ng.ProviderMediaKind.Image]


@pytest.mark.parametrize("stream", [case(s, keys=["OPENROUTER_API_KEY"], id=n) for n, s in MODES])
def test_openrouter_chat_keeps_generated_images(stream):
    model = "google/gemini-3.1-flash-lite-image"
    run = Run(provider("openrouter.chat", model), model, [user("A tiny flat red circle icon on white.")],
              stream=stream)
    images = _images(run)
    assert images
    assert images[0].mime.startswith("image/") and images[0].data


def _inline_bytes(run):
    return sum(len(part.data) for part in run.parts()
               if isinstance(part, ng.ProviderMedia) and part.source == ng.ProviderMediaSource.Inline and part.data)


@pytest.mark.parametrize("stream", [case(s, keys=["GEMINI_API_KEY"], id=n) for n, s in MODES])
def test_gemini_generate_image_output(stream):
    model = "gemini-3.1-flash-lite-image"
    run = Run(provider("google.generate", model), model,
              [user("A detailed watercolor painting of a harbour town at dawn, with boats, gulls and reflections.")],
              stream=stream)
    assert _inline_bytes(run) > 10_000


def _generate(*modalities, voice=None):
    generate = ng.GenerateControls()
    generate.response_modalities = list(modalities)
    if voice:
        speech = ng.GeminiSpeechConfig()
        speech.voice_config = ng.GeminiVoiceConfig()
        speech.voice_config.prebuilt_voice_name = voice
        generate.speech_config = speech
    return generate


def _audio(run):
    return [part for part in run.parts() if isinstance(part, ng.ProviderMedia) and part.kind == ng.ProviderMediaKind.Audio]


# Gemini speech, buffered and streamed (SchemaProvider#55): the Generate response modalities and speech settings
# reach the wire from Python (#512).
@pytest.mark.parametrize("stream", [case(s, keys=["GEMINI_API_KEY"], id=n) for n, s in MODES])
def test_gemini_tts(stream):
    model = "gemini-3.8-flash-lite-tts"
    run = Run(provider("google.generate", model), model, [user("Say: hello.")], stream=stream,
              generate=_generate(ng.GeminiResponseModality.Audio, voice="Kore"))
    audio = _audio(run)
    assert audio and audio[0].mime.startswith("audio/") and audio[0].data


# A Lyria clip on Generate, which needs its audio response modality.
@pytest.mark.parametrize("model", [case("lyria-3-clip-preview", keys=["GEMINI_API_KEY"], id="lyria-clip")])
def test_lyria_clip(model):
    run = Run(provider("google.generate", model), model,
              [user("A short bright chiptune loop. Instrumental only.")],
              generate=_generate(ng.GeminiResponseModality.Audio))
    assert _inline_bytes(run) > 0


# A Responses hosted tool from Python (SchemaProvider#54, #513): OpenAI web search, buffered and streamed.
# Responses cannot require a hosted tool yet (SchemaProvider#148), and the model sometimes answers this from memory.
# The case checks that a search, when the model makes one, decodes; so it asks up to three times for one.
@pytest.mark.parametrize("stream", [case(s, keys=["OPENAI_API_KEY"], id=n) for n, s in MODES])
def test_responses_hosted_web_search(stream):
    model, values = TEXT_MODELS["openai.responses"]
    responses = ng.ResponsesControls()
    responses.hosted_tools = [ng.ResponsesWebSearchTool()]
    for _attempt in range(3):
        run = Run(provider("openai.responses", model), model,
                  [user("Use web search: what is the title of the page at https://example.com? Answer in five words.")],
                  stream=stream, responses=responses, **{**values, "max_output_tokens": 1200})
        assert run.text().strip()
        searched = [part for part in run.parts() if isinstance(part, ng.ServerToolResult)] or \
            [citation for part in run.parts() if isinstance(part, ng.Text) for citation in part.citations]
        if searched:
            break
    assert searched, "the answer shows the web search: a server tool result or a citation"
