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


# Known gaps: they must fail; when one starts to pass, update tests/live/manifest.json. Both need the
# Generate response modalities and speech settings, which are not reachable from Python yet (#512).
GAPS = [
    case("lyria-3-clip-preview", "A short bright chiptune loop. Instrumental only.", keys=["GEMINI_API_KEY"],
         id="lyria-clip", gap="#512, Python cannot set response_modalities"),
    case("gemini-3.8-flash-lite-tts", "Say: hello.", keys=["GEMINI_API_KEY"], id="tts",
         gap="#512, Python cannot set speech_config"),
]


@pytest.mark.parametrize("model, prompt", GAPS)
def test_known_gap_gemini_media(model, prompt):
    run = Run(provider("google.generate", model), model, [user(prompt)])
    assert _inline_bytes(run) > 0
