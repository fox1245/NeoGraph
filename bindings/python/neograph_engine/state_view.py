"""Flat dot-access view of ``engine.get_state(thread_id)`` (v0.3.2).

The C++ engine's ``get_state`` returns a nested dict shaped like::

    {
      "channels": {
        "messages": {"value": [...], "version": 7, ...},
        "scratch":  {"value": "...", "version": 3, ...}
      },
      "global_version": 7
    }

That's faithful to the engine's internal representation but
``state["channels"]["messages"]["value"]`` is too deep for the
common read. ``StateView`` (and ``engine.get_state_view(thread_id)``)
collapse that to ``view.messages`` while keeping the raw shape one
``.raw`` away for callers that need version / metadata.

Pydantic v2 backs the implementation so users who care about typing
can subclass ``StateView`` with declared fields::

    class ChatState(StateView):
        messages: list[dict] = []
        scratch:  str = ""

    typed = engine.get_state_view("t1", model=ChatState)
    typed.messages  # → list[dict], type-checked
    typed.scratch   # → str

The base ``StateView`` allows arbitrary channel names via Pydantic's
``extra="allow"`` config, so ``view.<any_channel>`` works without a
declared model — handy for REPL / quick-debug flows.
"""

from __future__ import annotations

from typing import Any, Optional, Type

from pydantic import BaseModel, ConfigDict


class StateView(BaseModel):
    """Flat dot-access wrapper around the engine's get_state dict.

    Each channel from ``state["channels"]`` becomes a top-level
    attribute on the view, holding the channel's ``value``. The raw
    nested form is preserved on ``.raw`` so callers needing version /
    metadata don't lose information.

    Subclass with declared fields for typed access; instances of the
    base class accept any channel name via Pydantic's ``extra=allow``.

    Construct with :py:meth:`from_state` rather than the default
    pydantic constructor — ``from_state`` handles the
    ``state["channels"][n]["value"]`` flattening.
    """

    # Allow arbitrary channel names so the base class works without
    # users declaring fields. Subclasses can tighten this by overriding
    # ``model_config = ConfigDict(extra="forbid")`` for strict mode.
    model_config = ConfigDict(extra="allow")

    @classmethod
    def from_state(cls, state: dict) -> "StateView":
        """Build a view from the engine's nested state dict.

        ``state`` is the value returned by ``engine.get_state(thread_id)``
        (or ``None`` — handled by the engine wrapper, not here).
        Channels from ``state["channels"]`` become top-level attributes
        on the view.

        Raises ``ValueError`` if the input shape doesn't have a
        ``channels`` key — defensive against accidentally passing a
        ``RunResult.output`` slice or other un-flattened nested dict.
        """
        if not isinstance(state, dict) or "channels" not in state:
            raise ValueError(
                "StateView.from_state expects the engine's get_state() "
                "shape: {'channels': {<name>: {'value': ...}}, ...}; "
                f"got {type(state).__name__}")
        flat: dict[str, Any] = {}
        for name, ch in state.get("channels", {}).items():
            if isinstance(ch, dict) and "value" in ch:
                flat[name] = ch["value"]
            else:
                # Tolerate channel entries without the wrapper layer
                # (e.g. older serializations or test fixtures) — pass
                # through so the user still sees the data.
                flat[name] = ch
        view = cls(**flat)
        # Stash the raw dict so callers needing version / metadata can
        # reach it without a separate get_state call. Use a private
        # attribute name to avoid colliding with channel names.
        # Pydantic v2 doesn't allow writing arbitrary attrs on a model
        # instance after construction, so we store on the underlying
        # __dict__ via object.__setattr__.
        object.__setattr__(view, "_raw", state)
        return view

    @property
    def raw(self) -> dict:
        """The unflattened ``engine.get_state`` dict (for version /
        metadata access). Same object reference passed in to
        :py:meth:`from_state`, not a copy."""
        return getattr(self, "_raw", {})

    def channel_names(self) -> list[str]:
        """List of channel names available on this view."""
        # model_dump on extra=allow returns declared + extra fields.
        return [k for k in self.model_dump().keys() if not k.startswith("_")]

    def get(self, name: str, default: Any = None) -> Any:
        """Dict-like fallback access, useful when channel names are
        not known statically (e.g. ``view.get("messages")``)."""
        try:
            return getattr(self, name)
        except AttributeError:
            return default
