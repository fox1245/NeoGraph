"""``from_dict`` for ``ProviderControls`` and the SDK types of its family nests.

Each class below gets a strict ``from_dict(data)`` classmethod:

* an unknown key is a ``ValueError`` (a typo never reaches the provider);
* nested values are built by the field's type, and an object of the right class is taken as it is;
* an enum takes its member name (``"Audio"``) or the member itself;
* the variants (Responses hosted tools, Generate and Interactions built-in tools, Interactions response
  formats) pick their class from a ``"type"`` key, as the provider spells the tool or format.

``FIELDS`` is the table every ``from_dict`` reads. ``tests/test_provider_controls_from_dict.py`` checks it
against the bound properties, so a field added to the binding without the table fails the tests.
``neograph_engine.provider_endpoints`` installs its own table the same way (``install``).
"""

from __future__ import annotations

from typing import Any

from . import _neograph as _ng

_PAIRS = "pairs"  # a list of (name, value) string pairs
_JSON = "json"  # plain JSON data (a dict, list, number, string, bool or None)

HOSTED_TOOLS = {
    "web_search": "ResponsesWebSearchTool",
    "image_generation": "ResponsesImageGenerationTool",
    "file_search": "ResponsesFileSearchTool",
    "tool_search": "ResponsesToolSearchTool",
    "shell": "ResponsesShellTool",
    "code_interpreter": "ResponsesCodeInterpreterTool",
    "mcp": "ResponsesMcpTool",
    "computer": "ResponsesComputerTool",
    "computer_use_preview": "ResponsesComputerUsePreviewTool",
}
GENERATE_TOOLS = {
    "google_search": "GeminiGoogleSearchTool",
    "url_context": "GeminiUrlContextTool",
    "code_execution": "GeminiCodeExecutionTool",
    "google_maps": "GeminiGoogleMapsTool",
    "computer_use": "GeminiComputerUseTool",
    "file_search": "GeminiFileSearchTool",
}
INTERACTIONS_TOOLS = {
    "google_search": "InteractionsGoogleSearchTool",
    "url_context": "InteractionsUrlContextTool",
    "code_execution": "InteractionsCodeExecutionTool",
}
INTERACTIONS_FORMATS = {
    "audio": "InteractionsAudioResponseFormat",
    "image": "InteractionsImageResponseFormat",
    "video": "InteractionsVideoResponseFormat",
    "text": "InteractionsTextResponseFormat",
}

FIELDS: dict[str, dict[str, Any]] = {
    "ProviderControls": {
        "max_output_tokens": int, "max_tool_calls": int, "temperature": float, "top_p": float,
        "reasoning_effort": str, "reasoning_summary": str, "thinking_budget": int, "include_thoughts": bool,
        "thinking_level": str, "thinking_summaries": bool, "service_tier": str, "required_tool": str,
        "provider": "OpenRouterRouting", "response_format": "ProviderResponseFormat", "store": bool,
        "account_scope": str, "system": str,
        "chat": "ChatControls", "responses": "ResponsesControls", "messages": "MessagesControls",
        "generate": "GenerateControls", "interactions": "InteractionsControls",
    },
    "OpenRouterMaxPrice": {
        "prompt_nano_usd": int, "completion_nano_usd": int, "image_nano_usd": int, "audio_nano_usd": int,
        "request_nano_usd": int,
    },
    "OpenRouterRouting": {
        "zdr": bool, "allow_fallbacks": bool, "require_parameters": bool, "only": [str], "order": [str],
        "ignore": [str], "data_collection": str, "max_price": "OpenRouterMaxPrice",
    },
    "ProviderResponseFormat": {
        "kind": "ProviderResponseFormat.Kind", "name": str, "description": str, "schema": _JSON, "strict": bool,
    },
    "OpenRouterImageConfig": {
        "aspect_ratio": str, "image_size": str, "size": str, "quality": str, "background": str,
        "output_format": str, "moderation": str, "output_compression": int,
    },
    "OpenRouterPlugin": {"id": str, "options": _JSON},
    "ChatReasoningOptions": {"effort": str, "max_tokens": int, "exclude": bool, "enabled": bool},
    "ChatAudioOutput": {"voice": str, "format": str},
    "ChatControls": {
        "reasoning": "ChatReasoningOptions", "include_reasoning": bool, "usage_include": bool, "models": [str],
        "modalities": ["ChatOutputModality"], "audio": "ChatAudioOutput", "image_config": "OpenRouterImageConfig",
        "plugins": ["OpenRouterPlugin"], "extensions": _JSON,
    },
    "ResponsesApproximateLocation": {"city": str, "country": str, "region": str, "timezone": str},
    "ResponsesWebSearchTool": {
        "allowed_domains": [str], "user_location": "ResponsesApproximateLocation", "search_context_size": str,
        "external_web_access": bool,
    },
    "ResponsesImageGenerationTool": {
        "size": str, "quality": str, "output_format": str, "model": str, "action": str, "background": str,
        "moderation": str, "input_fidelity": str, "output_compression": int, "partial_images": int,
        "mask_file_id": str, "mask_image_url": str,
    },
    "ResponsesFileSearchTool": {"vector_store_ids": [str], "max_num_results": int},
    "ResponsesToolSearchTool": {},
    "ResponsesSkillReference": {"skill_id": str},
    "ResponsesContainerAutoEnvironment": {"skills": ["ResponsesSkillReference"]},
    "ResponsesShellTool": {"environment": "ResponsesContainerAutoEnvironment"},
    "ResponsesContainerNetworkPolicy": {"disabled": bool, "allowed_domains": [str]},
    "ResponsesCodeInterpreterTool": {
        "container_id": str, "file_ids": [str], "memory_limit": str,
        "network_policy": "ResponsesContainerNetworkPolicy",
    },
    "ResponsesMcpToolFilter": {"tool_names": [str], "read_only": bool},
    "ResponsesMcpTool": {
        "server_label": str, "server_url": str, "connector_id": str, "tunnel_id": str, "authorization": str,
        "headers": _PAIRS, "allowed_tools": "ResponsesMcpToolFilter", "require_approval": str,
        "approval_always": "ResponsesMcpToolFilter", "approval_never": "ResponsesMcpToolFilter",
        "server_description": str, "defer_loading": bool,
    },
    "ResponsesComputerTool": {},
    "ResponsesComputerUsePreviewTool": {"environment": str, "display_width": int, "display_height": int},
    "ResponsesOpenRouterServerTool": {"type": str, "parameters": _JSON},
    "ResponsesGenericHostedTool": {"type": str, "options": _JSON},
    "ResponsesMcpApproval": {"request_id": str, "approve": bool, "reason": str},
    "ResponsesControls": {
        "previous_response_id": str, "previous_response_history": ["ProviderMessage"],
        "parallel_tool_calls": bool, "verbosity": "ResponsesVerbosity", "truncation": "ResponsesTruncation",
        "include": ["ResponsesInclude"], "include_names": [str], "hosted_tools": [("variant", HOSTED_TOOLS)],
        "modalities": ["ResponsesOutputModality"], "image_config": "OpenRouterImageConfig",
        "plugins": ["OpenRouterPlugin"], "mcp_approvals": ["ResponsesMcpApproval"], "extensions": _JSON,
    },
    "MessagesCacheControl": {"ttl": "MessagesCacheTtl"},
    "MessagesToolChoice": {"mode": "MessagesToolChoiceMode", "name": str, "disable_parallel_tool_use": bool},
    "MessagesUserLocation": {"city": str, "region": str, "country": str, "timezone": str},
    "MessagesToolDefinition": {
        "name": str, "description": str, "input_schema": _JSON, "type": str, "max_uses": int,
        "allowed_callers": [str], "allowed_domains": [str], "blocked_domains": [str],
        "user_location": "MessagesUserLocation", "citations": bool, "max_content_tokens": int, "options": _JSON,
    },
    "MessagesMcpServer": {"name": str, "url": str, "authorization_token": str},
    "MessagesContainerSkill": {"type": str, "skill_id": str, "version": str},
    "MessagesControls": {
        "thinking_mode": "MessagesThinkingMode", "output_effort": "MessagesOutputEffort",
        "cache_control": "MessagesCacheControl", "tool_choice": "MessagesToolChoice",
        "tools": ["MessagesToolDefinition"], "container": str, "skills": ["MessagesContainerSkill"],
        "betas": [str], "mcp_servers": ["MessagesMcpServer"], "extensions": _JSON,
    },
    "GeminiSafetySetting": {"category": "GeminiSafetyCategory", "threshold": "GeminiSafetyThreshold"},
    "GeminiToolChoice": {"mode": "GeminiToolChoiceMode", "allowed_function_names": [str]},
    "GeminiVoiceConfig": {"prebuilt_voice_name": str, "voice": str},
    "GeminiSpeakerVoiceConfig": {"speaker": str, "voice_config": "GeminiVoiceConfig"},
    "GeminiSpeechConfig": {
        "voice_config": "GeminiVoiceConfig", "speaker_voice_configs": ["GeminiSpeakerVoiceConfig"],
        "language_code": str,
    },
    "GeminiImageConfig": {"aspect_ratio": str, "image_size": str},
    "GeminiAudioTranscriptionConfig": {
        "language_codes": [str], "custom_vocabulary": [str], "word_timestamp": bool, "diarization": bool,
        "mode": "GeminiTranscriptionMode",
    },
    "GeminiAudioFormat": {"mime_type": str, "sample_rate": int, "bit_rate": int, "delivery": str},
    "GeminiGoogleSearchTool": {"start_time": str, "end_time": str, "web_search": bool, "image_search": bool},
    "GeminiUrlContextTool": {},
    "GeminiCodeExecutionTool": {},
    "GeminiGoogleMapsTool": {"enable_widget": bool},
    "GeminiComputerUseTool": {
        "environment": str, "excluded_functions": [str], "disabled_safety_policies": [str],
        "prompt_injection_detection": bool,
    },
    "GeminiFileSearchTool": {"store_names": [str], "metadata_filter": str, "top_k": int},
    "GeminiMcpServer": {
        "name": str, "url": str, "headers": _PAIRS, "timeout": str, "sse_read_timeout": str,
        "terminate_on_close": bool,
    },
    "GeminiRetrievalConfig": {"latitude": float, "longitude": float, "language_code": str},
    "GenerateControls": {
        "history_mode": "GeminiHistoryMode", "thinking_level": "GeminiThinkingLevel",
        "safety_settings": ["GeminiSafetySetting"], "tool_choice": "GeminiToolChoice",
        "response_modalities": ["GeminiResponseModality"], "speech_config": "GeminiSpeechConfig",
        "image_config": "GeminiImageConfig", "audio_transcription_config": "GeminiAudioTranscriptionConfig",
        "audio_format": "GeminiAudioFormat", "builtin_tools": [("variant", GENERATE_TOOLS)],
        "mcp_servers": ["GeminiMcpServer"], "server_tool_invocations": bool,
        "retrieval_config": "GeminiRetrievalConfig", "extensions": _JSON,
    },
    "InteractionsAudioResponseFormat": {
        "mime_type": str, "sample_rate": int, "bit_rate": int, "delivery": "InteractionsDelivery",
    },
    "InteractionsImageResponseFormat": {
        "mime_type": str, "aspect_ratio": str, "image_size": str, "delivery": "InteractionsDelivery",
    },
    "InteractionsVideoResponseFormat": {
        "aspect_ratio": str, "resolution": str, "duration": str, "gcs_uri": str, "delivery": "InteractionsDelivery",
    },
    "InteractionsTextResponseFormat": {"mime_type": str, "schema": _JSON},
    "InteractionsSpeechConfig": {"voice": str, "speaker": str, "language": str},
    "InteractionsSpeakerConfig": {"speakers": ["InteractionsSpeechConfig"], "conversational": bool},
    "InteractionsTranscriptionConfig": {
        "language_codes": [str], "custom_vocabulary": [str], "mode": "InteractionsTranscriptionMode",
        "speaker_diarization": bool, "word_timestamps": bool,
    },
    "InteractionsGoogleSearchTool": {"search_types": [str]},
    "InteractionsUrlContextTool": {},
    "InteractionsCodeExecutionTool": {},
    "InteractionsControls": {
        "response_format": [("variant", INTERACTIONS_FORMATS)], "speech_config": "speech",
        "transcription_config": "InteractionsTranscriptionConfig",
        "builtin_tools": [("variant", INTERACTIONS_TOOLS)], "previous_interaction_id": str, "video_task": str,
        "agent": str, "background": bool, "extensions": _JSON,
    },
}


_TABLES: dict[str, dict[str, Any]] = {}  # every installed FIELDS table, by class name


def _resolve(name: str):
    target: Any = _ng
    for part in name.split("."):
        target = getattr(target, part)
    return target


def _is_enum(cls) -> bool:
    return hasattr(cls, "__members__")


def _build(name: str, value: Any, path: str):
    cls = _resolve(name)
    if isinstance(value, cls):
        return value
    fields = _TABLES.get(name)
    if fields is None or not isinstance(value, dict):
        expected = f"a dict or {name}" if fields is not None else f"a {name}"
        raise TypeError(f"{path}: expected {expected}, got {type(value).__name__}")
    result = cls()
    for key, item in value.items():
        if key not in fields:
            raise ValueError(f"{path}: unknown key {key!r} for {name}")
        setattr(result, key, _convert(fields[key], item, f"{path}.{key}"))
    return result


def _variant(table: dict[str, str], value: Any, path: str):
    known = {_resolve(name) for name in table.values()}
    known |= {_ng.ResponsesOpenRouterServerTool, _ng.ResponsesGenericHostedTool}
    if any(isinstance(value, cls) for cls in known):
        return value
    if not isinstance(value, dict) or not isinstance(value.get("type"), str):
        raise TypeError(f"{path}: expected a dict with a 'type' or a tool/format object")
    kind = value["type"]
    rest = {key: item for key, item in value.items() if key != "type"}
    if kind in table:
        return _build(table[kind], rest, path)
    if table is HOSTED_TOOLS:
        # A gateway's server tool, or a tool type the SDK policy admits as generic, keeps its type.
        name = "ResponsesOpenRouterServerTool" if kind.startswith("openrouter:") else "ResponsesGenericHostedTool"
        return _build(name, value, path)
    raise ValueError(f"{path}: unknown type {kind!r}; expected one of {sorted(table)}")


def _convert(spec: Any, value: Any, path: str):
    if value is None:
        return None
    if isinstance(spec, list):
        if not isinstance(value, (list, tuple)):
            raise TypeError(f"{path}: expected a list")
        return [_convert(spec[0], item, f"{path}[{index}]") for index, item in enumerate(value)]
    if isinstance(spec, tuple) and spec[0] == "variant":
        return _variant(spec[1], value, path)
    if spec == _JSON:
        return value
    if spec == _PAIRS:
        pairs = []
        for index, item in enumerate(value):
            if not (isinstance(item, (list, tuple)) and len(item) == 2 and all(isinstance(x, str) for x in item)):
                raise TypeError(f"{path}[{index}]: expected a (name, value) pair of strings")
            pairs.append((item[0], item[1]))
        return pairs
    if spec == "speech":
        # Interactions speech_config: a list of speech configs, or one speaker config.
        if isinstance(value, (list, tuple)):
            return [_build("InteractionsSpeechConfig", item, f"{path}[{index}]") for index, item in enumerate(value)]
        return _build("InteractionsSpeakerConfig", value, path)
    if isinstance(spec, str):
        target = _resolve(spec)
        if _is_enum(target):
            if isinstance(value, target):
                return value
            if isinstance(value, str) and value in target.__members__:
                return target.__members__[value]
            raise ValueError(f"{path}: expected one of {sorted(target.__members__)}")
        return _build(spec, value, path)
    if spec is bool:
        if not isinstance(value, bool):
            raise TypeError(f"{path}: expected a bool")
        return value
    if spec is int:
        if isinstance(value, bool) or not isinstance(value, int):
            raise TypeError(f"{path}: expected an int")
        return value
    if spec is float:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise TypeError(f"{path}: expected a number")
        return float(value)
    if spec is str:
        if not isinstance(value, str):
            raise TypeError(f"{path}: expected a str")
        return value
    if spec is bytes:
        if not isinstance(value, (bytes, bytearray)):
            raise TypeError(f"{path}: expected bytes")
        return bytes(value)
    raise AssertionError(f"{path}: no conversion for {spec!r}")


def install(fields: dict[str, dict[str, Any]]) -> None:
    """Gives each class of a FIELDS table its strict ``from_dict``, and lets the tables name each other's classes."""
    _TABLES.update(fields)
    for name in fields:
        cls = _resolve(name)

        def from_dict(cls, data, _name=name):
            """Build this value from a plain dict; unknown keys are refused."""
            return _build(_name, data, _name)

        cls.from_dict = classmethod(from_dict)


install(FIELDS)
