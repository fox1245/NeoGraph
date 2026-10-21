"""A scripted conversation through RuntimeTurnAssembler, offline, and how much of each request repeats the last (#329).

A provider's prompt cache reuses the start of a request that is identical, from its first byte, to an
earlier request. The runtime assembles every request from context artifacts and a RAW history, and the
family encoder lays the result out as one request body. This module runs a conversation of fixed length
through that path with a provider that never sends anything, keeps the encoded body of every call, and
measures the longest common prefix of each body with the one before it, in bytes and in approximate
tokens. The tests in test_prompt_prefix_wire.py assert on those measures.

The conversation has a fixed block (a RequiredSkill artifact), a RAW history that gains a question and a
reply per turn, and, depending on the layout, a small block that changes every turn. Nothing here is random:
the same arguments give the same bytes.
"""

import hashlib
import json
import random
import re
from collections import namedtuple

import neograph_engine as ng
from neograph_engine.llm import SchemaProvider

# preset -> (family, origin, buffered path, streaming path). The openrouter preset is the Chat family behind
# another origin.
PRESETS = {
    "openai.chat": ("openai.chat", "https://api.openai.com", "/v1/chat/completions", "/v1/chat/completions"),
    "openai.responses": ("openai.responses", "https://api.openai.com", "/v1/responses", "/v1/responses"),
    "anthropic.messages": ("anthropic.messages", "https://api.anthropic.com", "/v1/messages", "/v1/messages"),
    "google.generate": ("google.generate", "https://generativelanguage.googleapis.com",
                        "/v1beta/models/model:generateContent", "/v1beta/models/model:streamGenerateContent?alt=sse"),
    "openrouter.chat": ("openai.chat", "https://openrouter.ai", "/api/v1/chat/completions",
                        "/api/v1/chat/completions"),
}
# A history built from portable data has no native replay seal. Responses takes an assistant message only with one,
# so its trace holds the user's turns alone. Messages takes an assistant text, but a tool call only with a seal.
NO_ASSISTANT = {"openai.responses"}
NO_PORTABLE_TOOL_CALL = {"openai.responses", "anthropic.messages"}

OWNER = "trace-owner"
FEED = "trace-feed"
SESSION = "prefix-trace"
FIXED_ENTRIES = 115
DYNAMIC_LINES = 15
FIXED_PRIORITY = 100
SUMMARY_PRIORITY = 50
KEEP_PAIRS = 3

# What one layout puts into each call, besides the fixed block and the history.
#   volatile    (artifact kind, placement) of a block whose text changes every turn, or None
#   raw_volatile  the block is appended to the RAW feed as a user message instead of being an artifact
#   compact_at  turn at which the oldest exchanges leave the RAW window and a summary joins
#   tool_calls  parallel tool calls the model makes in every turn before it answers
Layout = namedtuple("Layout", "volatile raw_volatile compact_at tool_calls")
KIND = ng.ContextArtifactKind
PLACE = ng.ContextPlacement
LAYOUTS = {
    "stable-only": Layout(None, False, 0, 0),
    "append": Layout(None, True, 0, 0),
    "replace-tail": Layout((KIND.DerivedContext, PLACE.AfterHistory), False, 0, 0),
    "volatile-front": Layout((KIND.DerivedContext, PLACE.BeforeHistory), False, 0, 0),
    "before-latest-user": Layout((KIND.DerivedContext, PLACE.BeforeLatestUser), False, 0, 0),
    "system-hoist": Layout((KIND.HardConstraint, PLACE.AfterHistory), False, 0, 0),
    "tool-regroup": Layout(None, False, 0, 2),
    "compaction": Layout((KIND.DerivedContext, PLACE.AfterHistory), False, 10, 0),
}

Call = namedtuple("Call", "turn body items")
# One comparison of a call with the one before it. Ratios are shares of the new body; change_at is the
# index of the first prompt item (tools, system, then the messages) that differs from the previous call's, and
# before_last the byte range of the previous body's last message.
Pair = namedtuple("Pair", "turn bytes_ratio tokens_ratio common_bytes change_at items_before items_after "
                          "before_last")

ROWS = []  # summary rows of the traces run so far; the conftest hook prints them at the end of the session

PLACES = ["Harbor Quay", "North Ridge", "Cedar Flats", "Mill Creek", "Salt Marsh", "Iron Gate", "Fox Hollow",
          "Glass Bay", "High Meadow", "Stone Pier", "Birch Landing", "Copper Basin", "Dune Point", "Elm Crossing",
          "Frost Pass", "Gull Rock", "Heron Reach", "Ivy Terrace", "Juniper Hill", "Kelp Cove"]
KEEPERS = ["Maren Voss", "Tobias Lind", "Ilse Brandt", "Corin Hale", "Yara Ostrow", "Pavel Dunne", "Edda Marsh",
           "Niko Varga", "Selma Quist", "Anton Rhee", "Liesl Mohr", "Bram Tolliver"]
UNITS = ["kilopascals", "hectopascals", "millibars", "degrees Celsius", "millimetres per hour",
         "metres per second", "percent relative humidity", "lux", "parts per million"]
RULES = ["name the station before the figure", "flag any value outside the usual band as suspect",
         "quote the unit once and never abbreviate it", "repeat the keeper's name when a reading is disputed",
         "round to one decimal place and say so", "mention the previous shift's figure when it differs",
         "never estimate a missing reading", "state the quality flag after the figure"]


def sha_identity(text):
    return "sha256:" + hashlib.sha256(text.encode("utf-8")).hexdigest()


def fixed_text(entries=FIXED_ENTRIES):
    lines = [f"Session {SESSION}. You are the duty analyst of a network of weather stations. "
             "The reference bible below is authoritative. Answer in at most two short sentences."]
    for number in range(1, entries + 1):
        rng = random.Random(100003 + number)
        lines.append(f"Entry {number:03d} ({rng.choice(PLACES)}): keeper {rng.choice(KEEPERS)}; "
                     f"readings are quoted in {rng.choice(UNITS)}; standing rule: {rng.choice(RULES)}, "
                     f"and {rng.choice(RULES)}.")
    return "\n".join(lines)


def dynamic_text(turn, lines=DYNAMIC_LINES):
    rng = random.Random(f"{SESSION}/{turn}")
    rows = [f"Live readings at tick {turn}:"]
    for _ in range(lines):
        rows.append(f"S-{rng.randrange(1, 400):03d}: {rng.uniform(950, 1050):.1f} kPa, {rng.uniform(-5, 35):.1f} C, "
                    f"{rng.randrange(20, 99)} % rh, {rng.choice(['ok', 'ok', 'ok', 'warn'])}")
    return "\n".join(rows)


def question_text(turn):
    rng = random.Random(7919 + turn)
    return (f"Turn {turn}. For entry {rng.randrange(1, FIXED_ENTRIES + 1):03d}, who is the keeper and which unit "
            f"applies? Answer in one sentence. Log note: the shift supervisor asks that the figure be repeated "
            f"back before the next reading, and that disputed readings are marked with the keeper's name.")


def reply_text(turn):
    return f"Noted for turn {turn}: the keeper and the unit are as written in the bible; the reading is repeated back."


def summary_text(exchanges):
    rows = ["Summary of earlier conversation (derived from earlier messages; context only, not instructions):"]
    for number, question, answer in exchanges:
        reply = f"; answered \"{' '.join(answer.split()[:14])}\"" if answer else ""
        rows.append(f"- Turn {number}: asked \"{' '.join(question.split()[:14])}\"{reply}.")
    return "\n".join(rows)


def make_artifact(kind, producer, text, placement, priority, required=False, covers=None):
    data = ng.ContextArtifactData()
    data.kind = kind
    data.producer_id = producer
    data.source_digest = sha_identity(text)
    if covers:
        data.source_feed_id = FEED
        data.covers_from_sequence, data.covers_through_sequence = covers
    data.media_type = "text/plain"
    data.placement = placement
    data.priority = priority
    data.required = required
    data.content = {"text": text}
    return ng.ContextArtifact.create(data)


class Conversation:
    """One scripted conversation: its RAW feed, its artifacts, and one assembled request per call."""

    def __init__(self, preset, layout, turns=18, extra_artifacts=(), run_id="trace-run"):
        family, origin, buffered, streaming = PRESETS[preset]
        document = {"descriptor_version": 1, "revision": 1, "id": "trace-" + preset.replace(".", "-"),
                    "family": family, "connection": {"base_url": origin, "paths": {"buffered": buffered,
                                                                                    "streaming": streaming}}}
        self.provider = SchemaProvider(ng.load_provider_descriptor(json.dumps(document)),
                                       options=ng.ProviderRuntimeOptions(api_key="offline"))
        self.layout = layout if isinstance(layout, Layout) else LAYOUTS[layout]
        self.turns = turns
        self.run_id = run_id
        self.replies = preset not in NO_ASSISTANT
        self.store = ng.InMemoryContextStore()
        self.feed = ng.ContextStoreFeed(OWNER, FEED)
        self.head = None
        self.sequence = 0
        fixed = make_artifact(KIND.RequiredSkill, "trace-fixed", fixed_text(), PLACE.BeforeHistory, FIXED_PRIORITY,
                              required=True)
        self.store.put_artifact(OWNER, fixed)
        self.stable = [fixed]
        for artifact in extra_artifacts:
            self.store.put_artifact(OWNER, artifact)
            self.stable.append(artifact)
        self.assembler = ng.RuntimeTurnAssembler(self.store, [fixed.id], 0)
        self.calls = 0
        self.epoch = None
        self.window_start = 1
        self.summary = None
        self.exchanges = []
        self.exchange_ends = []
        self.bodies = []
        self.tools = []
        if self.layout.tool_calls:
            self.tools = [ng.ChatTool(name="read_station", description="Read one station.",
                                      parameters={"type": "object", "properties": {"station": {"type": "string"}},
                                                  "required": ["station"]})]
        controls = ng.ProviderControls()
        controls.max_output_tokens = 120
        if preset == "google.generate":
            controls.generate.history_mode = ng.GeminiHistoryMode.PortableForeign
        if preset == "openrouter.chat":
            controls.chat.usage_include = True
        self.controls = controls

    def append(self, role, parts, trust):
        self.sequence += 1
        data = ng.RuntimeHistoryRecordData()
        data.feed_id = FEED
        data.sequence = self.sequence
        data.message_id = f"message-{self.sequence}"
        data.trust = trust
        data.message = ng.ProviderMessage(role, parts)
        data.predecessor_id = self.head
        record = ng.RuntimeHistoryRecord.create(data)
        if self.store.append_history(self.feed, record, self.head) != ng.ContextStoreAppendResult.Appended:
            raise RuntimeError("the RAW feed refused a record")
        self.head = record.id
        return self.sequence

    def volatile_artifact(self, turn):
        kind, placement = self.layout.volatile
        required = kind == KIND.HardConstraint
        artifact = make_artifact(kind, "trace-live", dynamic_text(turn), placement, 0, required=required)
        self.store.put_artifact(OWNER, artifact)
        return artifact

    def compact(self):
        summarized = self.exchanges[:-KEEP_PAIRS]
        last = self.exchange_ends[len(summarized) - 1]
        summary = make_artifact(KIND.DerivedContext, "trace-summary", summary_text(summarized), PLACE.BeforeHistory,
                                SUMMARY_PRIORITY, covers=(self.window_start, last))
        self.store.put_artifact(OWNER, summary)
        self.summary = summary
        self.window_start = last + 1

    def provider_call(self, turn):
        artifacts = list(self.stable)
        if self.layout.volatile:
            artifacts.append(self.volatile_artifact(turn))
        if self.summary is not None:
            artifacts.append(self.summary)
        self.calls += 1
        data = ng.ContextEpochData()
        data.run_id = self.run_id
        data.sequence = self.calls
        data.predecessor_id = self.epoch.id if self.epoch is not None else None
        data.feed_id = FEED
        data.raw_from_sequence = self.window_start
        data.raw_through_sequence = self.sequence
        data.raw_window_digest = self.store.snapshot_history(self.feed, self.window_start, self.sequence).digest
        data.artifact_ids = [artifact.id for artifact in artifacts]
        data.guarantee_profile = ng.RuntimeGuaranteeProfile.Recorded
        self.epoch = ng.ContextEpoch.create(data)
        request = ng.make_provider_request(self.provider, "model", [], tools=self.tools, controls=self.controls,
                                           mode=ng.ProviderMode.Collect)
        prepared, _ = self.assembler.assemble(self.provider, OWNER, self.epoch, request)
        if not prepared.valid:
            raise RuntimeError(f"the provider refused the request: {prepared.error}")
        body = prepared.encoded_body
        self.bodies.append(Call(turn, body, logical_items(body)))
        return body

    def run(self):
        for turn in range(1, self.turns + 1):
            if self.layout.compact_at and turn == self.layout.compact_at:
                self.compact()
            if self.layout.raw_volatile:
                self.append(ng.ProviderRole.User, [ng.Text(dynamic_text(turn))], ng.RuntimeTrustClass.UntrustedInput)
            question = question_text(turn)
            self.append(ng.ProviderRole.User, [ng.Text(question)], ng.RuntimeTrustClass.UntrustedInput)
            if self.layout.tool_calls:
                self.tool_round(turn)
            self.provider_call(turn)
            answer = reply_text(turn) if self.replies else None
            if answer is not None:
                self.append(ng.ProviderRole.Assistant, [ng.Text(answer)], ng.RuntimeTrustClass.ModelOutput)
            self.exchanges.append((turn, question, answer))
            self.exchange_ends.append(self.sequence)
        return self.bodies

    def tool_round(self, turn):
        """A call that ends in parallel tool calls, then their results as one RAW record each."""
        self.provider_call(turn)
        calls = []
        for number in range(self.layout.tool_calls):
            call = ng.ProviderToolCall()
            call.id, call.name = f"call-{turn}-{number}", "read_station"
            call.input = {"station": f"S-{number:03d}"}
            calls.append(call)
        self.append(ng.ProviderRole.Assistant, calls, ng.RuntimeTrustClass.ModelOutput)
        for call in calls:
            result = ng.ProviderToolResult(call.id, '{"pressure_kpa": 1013.2, "flag": "ok"}')
            result.host = ng.ToolResultHostMetadata()
            result.host.name = call.name
            self.append(ng.ProviderRole.Tool, [result], ng.RuntimeTrustClass.ToolOutput)


def run_trace(preset, layout, turns=18):
    return Conversation(preset, layout, turns).run()


# What marks each artifact of sibling_bodies in the text of a prompt.
SIBLING_MARKERS = {"alpha": "Standing note alpha", "bravo": "Standing note bravo", "live": "Live readings"}


def sibling_bodies(preset, kind, mutations=100):
    """Bodies of consecutive calls over one history, with two artifacts that stay as they are and a third whose text
    changes on every call. The three share a kind, a producer, a placement and a priority, so only the tie-break
    among equals decides their order."""
    required = kind == KIND.HardConstraint
    fixed = [make_artifact(kind, "trace-live", f"Standing note {name}: " + f"{name} stays as written. " * 12,
                           PLACE.BeforeHistory, 0, required=required)
             for name in ("alpha", "bravo")]
    conversation = Conversation(preset, Layout((kind, PLACE.BeforeHistory), False, 0, 0), extra_artifacts=fixed)
    conversation.append(ng.ProviderRole.User, [ng.Text(question_text(1))], ng.RuntimeTrustClass.UntrustedInput)
    return [conversation.provider_call(turn) for turn in range(1, mutations + 1)]


def sibling_order(body):
    """The three artifacts of sibling_bodies as names, in the order the prompt carries them."""
    prompt = "\n".join(text for _, text in logical_items(body))
    where = {name: prompt.index(marker) for name, marker in SIBLING_MARKERS.items()}
    return tuple(sorted(where, key=where.get))


# --- reading a body -----------------------------------------------------------------------------

TEXT_PARTS = ("text", "input_text", "output_text")

def content_text(content):
    """The text of a message or system field; content that is not plain text is read as sorted JSON."""
    if isinstance(content, str):
        return content
    if isinstance(content, list) and content and all(
            isinstance(part, dict) and "text" in part and part.get("type", "text") in TEXT_PARTS
            for part in content):
        return "".join(part["text"] for part in content)
    return json.dumps(content, sort_keys=True, ensure_ascii=False)


def logical_items(body):
    """The request in the order a provider reads it: tools, system, then the messages."""
    document = json.loads(body)
    items = []
    if document.get("tools"):
        items.append(("tools", content_text(document["tools"])))
    for key in ("system", "instructions", "systemInstruction", "system_instruction"):
        if document.get(key):
            items.append(("system", content_text(document[key])))
    for key in ("messages", "input", "contents"):
        for message in document.get(key) or []:
            if isinstance(message, dict):
                role = message.get("role", message.get("type", "?"))
                content = message.get("content", message.get("parts", message))
            else:
                role, content = "?", message
            items.append((role, content_text(content)))
    return items


def message_spans(body):
    """Byte range of every element of the body's message array, found without reordering the body."""
    for key in ("messages", "input", "contents"):
        marker = f'"{key}":['
        start = body.find(marker)
        if start >= 0:
            break
    else:
        return []
    decoder = json.JSONDecoder()
    position = start + len(marker)
    spans = []
    while body[position] != "]":
        _, end = decoder.raw_decode(body, position)
        spans.append((len(body[:position].encode("utf-8")), len(body[:end].encode("utf-8"))))
        position = end + (body[end] == ",")
    return spans


def label(role, text):
    """A short name for a prompt item of this trace: F fixed block, V live block, Q question, A reply, T tool."""
    mark = ("F" if "Entry 001" in text else "") + ("V" if "Live readings" in text else "")
    if mark:
        return mark if role not in ("system",) else "system[" + mark + "]"
    if "Summary of earlier" in text:
        return "C"
    if "read_station" in text or "tool_" in text or "functionCall" in text or "function_call" in text:
        return "T"
    if text.startswith("Turn "):
        return "Q"
    if text.startswith("Noted for turn"):
        return "A"
    return role[:1].upper()


def order_of(call):
    """The prompt items of a call as labels, for example F Q A Q V."""
    return " ".join(label(role, text) for role, text in call.items)


# --- measures -----------------------------------------------------------------------------------

PIECE = re.compile(r" ?[A-Za-z]+| ?[0-9]+| ?[^A-Za-z0-9\s]+|\s+")


def pieces(text):
    """An approximate token stream: a word, a number or a run of punctuation, with its leading space.

    A byte-pair tokenizer splits a long word further, so this undercounts tokens, but a prefix that two
    texts share is measured the same way in both and the ratio of the two counts is a fair estimate."""
    return PIECE.findall(text)


def common_prefix(left, right):
    low, high = 0, min(len(left), len(right))
    while low < high:
        middle = (low + high + 1) // 2
        if left[:middle] == right[:middle]:
            low = middle
        else:
            high = middle - 1
    return low


def common_pieces(left, right):
    count = 0
    for one, other in zip(left, right):
        if one != other:
            break
        count += 1
    return count


def first_change(before, after):
    for index, (one, other) in enumerate(zip(before, after)):
        if one != other:
            return index
    return min(len(before), len(after))


def compare(before, after):
    old, new = before.body.encode("utf-8"), after.body.encode("utf-8")
    common = common_prefix(old, new)
    old_pieces, new_pieces = pieces(before.body), pieces(after.body)
    spans = message_spans(before.body)
    return Pair(after.turn, common / len(new), common_pieces(old_pieces, new_pieces) / len(new_pieces), common,
                first_change(before.items, after.items), len(before.items), len(after.items),
                spans[-1] if spans else None)


def compare_all(calls):
    return [compare(before, after) for before, after in zip(calls, calls[1:])]


def mean(values):
    values = list(values)
    return sum(values) / len(values)


def summary_row(preset, layout, pairs):
    """One line of the table: ratios over the calls after the first, and how many calls only extend."""
    extends = sum(1 for pair in pairs if pair.change_at == pair.items_before)
    tail = sum(1 for pair in pairs if pair.change_at == pair.items_before - 1)
    front = sorted({pair.change_at for pair in pairs})
    return (preset, layout, len(pairs) + 1,
            f"{100 * mean(p.bytes_ratio for p in pairs):.0f}% / {100 * min(p.bytes_ratio for p in pairs):.0f}%",
            f"{100 * mean(p.tokens_ratio for p in pairs):.0f}% / {100 * min(p.tokens_ratio for p in pairs):.0f}%",
            f"{extends} / {tail} / {len(pairs) - extends - tail}", ",".join(map(str, front[:6])))


TABLE_HEAD = ("family", "layout", "calls", "bytes mean / min", "tokens mean / min",
              "extends / tail only / other", "first changed item")


def render_table(rows):
    widths = [max(len(str(row[column])) for row in (TABLE_HEAD, *rows)) for column in range(len(TABLE_HEAD))]
    lines = ["  ".join(str(cell).ljust(width) for cell, width in zip(row, widths)).rstrip()
             for row in (TABLE_HEAD, *rows)]
    lines.insert(1, "  ".join("-" * width for width in widths))
    return lines
