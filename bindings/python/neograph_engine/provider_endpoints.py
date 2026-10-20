"""``from_dict`` for the request classes of ``Provider.prepare_endpoint`` and ``complete_endpoint``.

Each request class, and the value classes nested in one (``EmbeddingInput``, ``JobHandle``, ``FileSource``,
``FileUploadSession``, ...), gets the strict ``from_dict(data)`` of ``neograph_engine.provider_controls``: an unknown
key is a ``ValueError``, nested values are built by the field's type, and an enum takes its member name
(``"Choice"``) or the member itself. Result classes have no ``from_dict``.

Media (``ProviderMedia``), a ``Batch`` and a ``ProviderRequest`` have no dict form; pass the object. A
``BatchItem`` or ``TokenCount`` takes its ``request`` from a ``ProviderRequest``.

``FIELDS`` is the table every ``from_dict`` here reads. ``tests/test_provider_endpoints.py`` checks it against the
bound properties, so a field added to the binding without the table fails the tests.
"""

from __future__ import annotations

from typing import Any

from .provider_controls import _JSON, install

FIELDS: dict[str, dict[str, Any]] = {
    "ImagesRequest": {
        "model": str, "prompt": str, "references": ["ProviderMedia"], "count": int, "size": str, "quality": str,
        "background": str, "output_format": str, "output_compression": int, "extensions": _JSON,
    },
    "DecisionOption": {"value": str, "guidance": _JSON},
    "DecisionQuestion": {
        "name": str, "kind": "DecisionKind", "instructions": _JSON, "options": ["DecisionOption"],
        "when_true": _JSON, "when_false": _JSON,
    },
    "DecisionsRequest": {
        "model": str, "input": _JSON, "questions": ["DecisionQuestion"], "extensions": _JSON,
    },
    "EmbeddingInput": {"text": str, "media": ["ProviderMedia"]},
    "EmbeddingsRequest": {
        "model": str, "inputs": ["EmbeddingInput"], "dimensions": int, "task": str, "extensions": _JSON,
    },
    "RerankRequest": {"model": str, "query": str, "documents": [str], "top": int, "extensions": _JSON},
    "ModerationInput": {"text": str, "media": ["ProviderMedia"]},
    "ModerationRequest": {"model": str, "inputs": ["ModerationInput"], "extensions": _JSON},
    "SpeechRequest": {
        "model": str, "input": str, "voice": str, "instructions": str, "format": str, "speed": float,
        "extensions": _JSON,
    },
    "TranscriptionRequest": {
        "model": str, "audio": "ProviderMedia", "language": str, "prompt": str, "translate": bool,
        "extensions": _JSON,
    },
    "VideoRequest": {
        "model": str, "prompt": str, "references": ["ProviderMedia"], "duration": int, "aspect_ratio": str,
        "resolution": str, "size": str, "audio": bool, "extensions": _JSON,
    },
    "JobHandle": {"endpoint": str, "origin": str, "id": str, "model": str},
    "JobPoll": {"handle": "JobHandle"},
    "JobDownload": {"handle": "JobHandle", "index": int, "path": str, "max_bytes": int},
    "FileSource": {"bytes": bytes, "path": str, "offset": int, "size": int, "sha256": str},
    "FileUpload": {
        "source": "FileSource", "filename": str, "mime": str, "purpose": str, "expires_after": int,
    },
    "FileGet": {"id": str},
    "FileDelete": {"id": str},
    "FileList": {"limit": int, "after": str, "purpose": str},
    "FileDownload": {"id": str, "path": str, "max_bytes": int},
    "FileUploadSession": {
        "origin": str, "shape": str, "id": str, "url": str, "size": int, "sent": int, "granularity": int,
        "parts": [str], "expires_at": int, "status": str,
    },
    "FileUploadStart": {"filename": str, "mime": str, "size": int, "purpose": str, "expires_after": int},
    "FileUploadPart": {"session": "FileUploadSession", "source": "FileSource"},
    "FileUploadComplete": {"session": "FileUploadSession"},
    "FileUploadQuery": {"session": "FileUploadSession"},
    "FileUploadCancel": {"session": "FileUploadSession"},
    "BatchItem": {"custom_id": str, "request": "ProviderRequest"},
    "BatchSubmit": {
        "items": ["BatchItem"], "input_file": str, "endpoint": str, "display_name": str, "extensions": _JSON,
    },
    "BatchGet": {"id": str},
    "BatchCancel": {"id": str},
    "BatchDelete": {"id": str},
    "BatchResults": {"batch": "Batch", "path": str, "max_bytes": int, "errors": bool},
    "TokenCount": {"request": "ProviderRequest"},
    "GenerationGet": {"id": str},
}

install(FIELDS)
