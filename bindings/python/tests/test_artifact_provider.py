import neograph_engine as ng


def test_generated_artifact_values_are_public_and_preserve_lifecycle_fields():
    artifact = ng.Artifact(
        identity="image-1",
        uri="artifact://image-1",
        media_type="image/png",
        size_bytes=42,
        metadata={"width": 512},
    )
    assert artifact.identity == "image-1"
    assert artifact.metadata == {"width": 512}

    poll = ng.ArtifactPollPolicy(max_attempts=3, interval_ms=25)
    request = ng.ArtifactRequest(
        operation="generate-image",
        input={"prompt": "sunrise"},
        mode=ng.ArtifactExecutionMode.LongRunning,
        idempotency_key="request-1",
        poll=poll,
    )
    assert request.input == {"prompt": "sunrise"}
    assert request.mode == ng.ArtifactExecutionMode.LongRunning
    assert request.poll.max_attempts == 3
    assert request.poll.interval_ms == 25

    operation = ng.ArtifactOperation()
    operation.id = "operation-1"
    operation.status = ng.ArtifactOperationStatus.Succeeded
    operation.artifacts = [artifact]
    operation.output = {"provider_request_id": "remote-1"}

    assert operation.terminal()
    assert operation.succeeded()
    assert not operation.failed()
    assert operation.artifacts[0].uri == "artifact://image-1"
    assert operation.output == {"provider_request_id": "remote-1"}
