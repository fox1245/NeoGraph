"""Typed provider integration for builds with the native LLM target.

Core-only builds retain the real request factory and transport options here;
``SchemaProvider`` and its defaults are exported only when compiled natively.
There is no Python substitute for an omitted native provider implementation.
"""

from ._neograph import (
    load_provider_descriptor,
    make_provider_request,
    ProviderDeploymentHeaderEnvironment,
    load_provider_descriptor_with_environment_headers,
    load_provider_descriptor_with_deployment_headers,
)
from . import ProviderRuntimeOptions, _HAVE_LLM

__all__ = [
    "ProviderRuntimeOptions", "load_provider_descriptor", "make_provider_request",
    "ProviderDeploymentHeaderEnvironment",
    "load_provider_descriptor_with_environment_headers",
    "load_provider_descriptor_with_deployment_headers",
]

if _HAVE_LLM:
    from ._neograph import SchemaProvider as _SchemaProvider, SchemaProviderDefaults

    class SchemaProvider(_SchemaProvider):
        """Typed provider using the package CA bundle when options are omitted."""

        def __init__(self, descriptor, options=None, defaults=None):
            if options is None:
                options = ProviderRuntimeOptions()
            if defaults is None:
                defaults = SchemaProviderDefaults()
            super().__init__(descriptor, options, defaults)

    __all__.extend(["SchemaProvider", "SchemaProviderDefaults"])
