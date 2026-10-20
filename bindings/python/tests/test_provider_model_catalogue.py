"""The embedded SchemaProvider model catalogue is exported from the package, next to the embedded policy."""

import json

import neograph_engine as ng


def test_provider_model_catalogue_json_is_exported():
    assert "provider_model_catalogue_json" in ng.__all__
    catalogue = json.loads(ng.provider_model_catalogue_json())
    assert catalogue["version"] == 1
    assert isinstance(catalogue["models"], list)


def test_the_exported_catalogue_loads_as_a_policy_layer():
    layered = ng.load_provider_policy(ng.provider_policy_json(), ng.provider_codec_defaults_json(),
                                      ng.provider_model_catalogue_json())
    assert layered.identity == ng.builtin_provider_policy().identity
