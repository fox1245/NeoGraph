# Third-party licenses

NeoGraph is [MIT-licensed](LICENSE). Third-party components keep their own
licenses. [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES) contains the license
texts and attributions for the components below; ship it with this index
and `LICENSE` when redistributing the engine or Python wheels.

## Source and compiled components

| Component | Source or upstream license | License | Distribution / use |
|---|---|---|---|
| SchemaProvider | [SDK LICENSE](https://github.com/fox1245/SchemaProvider/blob/main/LICENSE) | MIT, Copyright (c) 2026 fox1245 | SDK implementation linked into NeoGraph |
| yyjson | [`deps/yyjson/LICENSE`](deps/yyjson/LICENSE) | MIT | Vendored C implementation compiled into the engine |
| standalone Asio | [`COPYING`](deps/asio/COPYING), [`LICENSE_1_0.txt`](deps/asio/LICENSE_1_0.txt) | Boost Software License 1.0 | Vendored headers for the engine and SDK coroutine / network runtime |
| cpp-httplib | [Upstream LICENSE](https://github.com/yhirose/cpp-httplib/blob/master/LICENSE); `deps/httplib.h` | MIT | Vendored headers for NeoGraph HTTP transports; both upstream and vendored-header copyright notices are retained |
| moodycamel::ConcurrentQueue | [`deps/concurrentqueue.h`](deps/concurrentqueue.h) | BSD 2-Clause, also offered under Boost 1.0 | Vendored queue; BSD notice reproduced in the full notices |
| QuickJS | [`deps/quickjs/LICENSE`](deps/quickjs/LICENSE), source-file headers | MIT | Vendored engine for the optional JavaScript Program control runtime; includes per-file attributions |
| Unicode data | [Unicode License V3](https://www.unicode.org/license.txt) | Unicode-3.0 | Generated Unicode tables in QuickJS |
| pybind11 | [Upstream LICENSE](https://github.com/pybind/pybind11/blob/v2.13.6/LICENSE) | BSD 3-Clause | Header code compiled into the Python extension; CMake fallback version 2.13.6, Python builds may select a newer compatible version |
| SHA-Intrinsics adaptation | [Pinned source notice](https://github.com/noloader/SHA-Intrinsics/blob/d03795497f3e4576083fc2cd8fe0b924f24d0bb2/sha256-x86.c) | Public domain | SHA intrinsic register layout in `src/core/sha256.cpp`; attribution to Jeffrey Walton, Intel and Sean Gulley / miTLS |
| cppdotenv | [`deps/cppdotenv/LICENSE`](deps/cppdotenv/LICENSE) | MIT | Vendored example `.env` reader |
| Clay | License at the end of [`deps/clay.h`](deps/clay.h) | zlib | Vendored optional example UI and `deps/clay_renderer_raylib.c` glue |
| raylib | [Upstream LICENSE](https://github.com/raysan5/raylib/blob/5.5/LICENSE) | zlib | Fetched for the optional Clay example, not a wheel build dependency |

## External libraries and repaired wheels

Source builds resolve these libraries from the build environment. Wheel
repair tools can copy their shared libraries, and additional transitive
libraries, into the wheel. A library installed through the operating system,
vcpkg or Homebrew can therefore become a bundled wheel component. Dynamic
linking alone does not determine whether it is distributed.

| Component | Upstream license | License | Use |
|---|---|---|---|
| curl / libcurl | [curl 8.22.0 COPYING](https://github.com/curl/curl/blob/curl-8_22_0/COPYING) | curl license | SchemaProvider HTTP transport and optional NeoGraph HTTP/2 pool |
| nghttp2 | [COPYING](https://github.com/nghttp2/nghttp2/blob/master/COPYING) | MIT | HTTP/2 support in libcurl |
| OpenSSL 3 | [LICENSE.txt](https://github.com/openssl/openssl/blob/openssl-3.5/LICENSE.txt) | Apache 2.0 | SDK cryptography, TLS in libcurl and NeoGraph transports |
| PostgreSQL / libpq | [COPYRIGHT](https://github.com/postgres/postgres/blob/REL_17_STABLE/COPYRIGHT) | PostgreSQL license | `neograph::postgres` checkpoint storage |
| SQLite | [Public-domain dedication](https://www.sqlite.org/copyright.html) | Public domain | `neograph::sqlite` checkpoint storage and durable Program state |
| zlib | [LICENSE](https://github.com/madler/zlib/blob/v1.3.1/LICENSE) | zlib | Compression support selected by external library builds |

The Linux wheel configuration builds curl 8.22.0 with OpenSSL, system
nghttp2 and zlib. Windows resolves dependencies through vcpkg; macOS uses
Homebrew. Package versions and transitive libraries vary across those
platforms. The tables identify the declared components, not a complete
inventory of every repaired wheel. Keep the actual artifact's dependency
inventory with the release and include the license notices for any additional
libraries it contains.

## Platform-resolved transitive libraries

The full notices also reproduce package copyright metadata for the following
libraries observed in a Linux qualification build. These entries apply when
a distribution contains the library. They do not assert that every wheel
contains it, or that another platform uses the same package version.

| Component | Upstream | Library license / included notices |
|---|---|---|
| MIT Kerberos (`krb5`, `k5crypto`, `krb5support`, GSSAPI) | [MIT Kerberos](https://web.mit.edu/kerberos/) | BSD-style and per-file permissive notices |
| Cyrus SASL | [Cyrus SASL](https://www.cyrusimap.org/sasl/) | BSD-style with Carnegie Mellon attribution and per-file notices |
| OpenLDAP (`ldap`, `lber`) | [OpenLDAP](https://www.openldap.org/) | OpenLDAP Public License 2.8 and per-file notices |
| GnuTLS | [GnuTLS](https://www.gnutls.org/) | LGPL 2.1-or-later library; transitive dependencies have separate terms |
| libidn2 | [GNU Libidn](https://www.gnu.org/software/libidn/) | LGPL 3-or-later or GPL 2-or-later library; Unicode data notice |
| libssh | [libssh](https://www.libssh.org/) | LGPL 2.1-or-later with OpenSSL exception; some files LGPL 2.1 or permissive |
| Nettle / Hogweed | [Nettle](https://www.lysator.liu.se/~nisse/nettle/) | LGPL 3-or-later or GPL 2-or-later; per-file permissive notices |
| libtasn1 | [GNU Libtasn1](https://www.gnu.org/software/libtasn1/) | LGPL 2.1-or-later library |
| p11-kit | [p11-kit](https://p11-glue.github.io/p11-glue/p11-kit.html) | BSD 3-Clause, ISC and per-file notices |
| keyutils | [keyutils](https://git.kernel.org/pub/scm/linux/kernel/git/dhowells/keyutils.git/) | LGPL 2-or-later library |
| libffi | [libffi](https://github.com/libffi/libffi) | MIT / Expat library and per-file notices |

Section 19 of the full notices records the inspected Ubuntu package versions
and preserves their file-specific copyright and license declarations.
Package source metadata also describes tools, tests and documentation;
those declarations retain their original file scope and do not relicense
unrelated NeoGraph code. Use the applicable notices from the exact package
when a different build supplies these libraries.

## Declared Homebrew closure (not artifact-qualified)

The [curl formula](https://github.com/Homebrew/homebrew-core/blob/cb210f70f163a3a1bdf15c6b2016730cbecb18a4/Formula/c/curl.rb)
at Homebrew/core commit `cb210f70f163a3a1bdf15c6b2016730cbecb18a4`
declares the following additional dependencies. Versions below are that
formula snapshot's stable source versions, not versions verified in a released
NeoGraph wheel. Section 20 of the full notices reproduces their legal texts
and records matching source archives and formula provenance.

| Component / snapshot version | Matching upstream legal document | License / scope |
|---|---|---|
| Brotli 1.2.0 | [LICENSE](https://github.com/google/brotli/blob/v1.2.0/LICENSE) | MIT; compression libraries |
| nghttp3 1.18.0 (`libnghttp3`) | [COPYING](https://github.com/ngtcp2/nghttp3/blob/v1.18.0/COPYING) | MIT; HTTP/3 library |
| ngtcp2 1.25.0 (`libngtcp2`) | [COPYING](https://github.com/ngtcp2/ngtcp2/blob/v1.25.0/COPYING) | MIT; QUIC and crypto adapter libraries |
| libpsl 0.23.3 | [LICENSE](https://github.com/rockdaboot/libpsl/blob/0.23.3/LICENSE), [Chromium notice](https://github.com/rockdaboot/libpsl/blob/0.23.3/src/LICENSE.chromium) | MIT and BSD 3-Clause code; built-in Public Suffix List separately MPL 2.0 |
| libssh2 1.11.1, Homebrew revision 6 | [COPYING](https://github.com/libssh2/libssh2/blob/libssh2-1.11.1/COPYING) | BSD 3-Clause; distinct from LGPL-licensed libssh; retain formula patches with source provenance |
| Zstandard 1.5.7, Homebrew revision 1 | [LICENSE](https://github.com/facebook/zstd/blob/v1.5.7/LICENSE), [dictionary-builder notice](https://github.com/facebook/zstd/blob/v1.5.7/lib/dictBuilder/divsufsort.c) | BSD 3-Clause alternative selected here, plus Yuta Mori's MIT notice; not a GPL-only library |
| LZ4 1.10.0 | [Library LICENSE](https://github.com/lz4/lz4/blob/v1.10.0/lib/LICENSE) | BSD 2-Clause library; declared by the zstd formula for program support, not proven bundled |
| XZ Utils 5.8.4 / liblzma | [COPYING](https://github.com/tukaani-project/xz/blob/v5.8.4/COPYING), [COPYING.0BSD](https://github.com/tukaani-project/xz/blob/v5.8.4/COPYING.0BSD) | 0BSD library; package scripts separately GPL 2-or-later; declared zstd program dependency, not proven bundled |
| Public Suffix List | [Pinned list source](https://github.com/publicsuffix/list/blob/e1b8015c3b2f0f4f8c18659c2480fc1a22c07b20/public_suffix_list.dat), [LICENSE](https://github.com/publicsuffix/list/blob/e1b8015c3b2f0f4f8c18659c2480fc1a22c07b20/LICENSE) | MPL 2.0 data; this commit is libpsl 0.23.3's source-tag submodule, not an inspected bottle's data identity |

On macOS 14 and newer, this libpsl formula selects system `libicucore`,
not libidn2/libunistring; curl selects Apple IDN and system Kerberos/OpenLDAP.
Its Linux/older-macOS alternatives do not describe NeoGraph's Linux
source-built curl recipe. The zstd formula enables LZ4/LZMA support for its
programs: a package dependency is not proof of a shared-library dependency.
Do not include system libraries in a bundled inventory merely because they
appear in package metadata.

Before publication, inspect each repaired wheel and portable archive on all
four platform rows, including static incorporation. Record actual versions,
formula/port/package revisions, source checksums, patches and copied libraries;
retain matching notices for any version not represented here. For bundled
libpsl's built-in list, verify the actual list digest/date against its build
source. Make that matching MPL-covered source, including modifications,
available and tell recipients where to obtain it (MPL 2.0 section 3.2).
The pinned public source above is not proof that an uninspected binary uses it.

## Redistribution requirements

Preserve the copyright notices, permission notices and disclaimers in the
full notices and upstream sources. BSD-licensed binary distributions must
reproduce their notices in accompanying materials. Include the Apache 2.0
license and any applicable upstream `NOTICE` attributions when distributing
OpenSSL 3; mark modified source files as required by that license. The curl
and BSD licenses restrict use of upstream names for promotion or endorsement.
The zlib licenses require altered source versions to be marked and prohibit
misrepresenting their origin. Consult the complete license texts for the
conditions that apply to each component.

Distributing LGPL libraries also requires the source and linking arrangements
specified by the applicable LGPL version. Provide matching library source,
package patches and build material through a permitted distribution method.
Allow replacement or relinking of the LGPL portions and reverse engineering
for debugging their modifications as required by the license. A notice alone
does not satisfy those conditions. The full notices include LGPL 2.0, 2.1,
3.0 and GPL 2.0 / 3.0; LGPL 3.0 incorporates GPL 3.0 terms.

The public Ubuntu source-package pages below provide the original source,
package patches/build rules and `.dsc` checksums for Section 19's recorded
versions. They are matching-package source locations, not a claim that
release-matrix artifacts contain these Ubuntu binaries or that NeoGraph has
completed a license-compliant source distribution.

| Section 19 component | Matching public package source |
|---|---|
| GnuTLS | [gnutls28 3.8.3-1.1ubuntu3.6](https://launchpad.net/ubuntu/+source/gnutls28/3.8.3-1.1ubuntu3.6) |
| libidn2 | [2.3.7-2build1.1](https://launchpad.net/ubuntu/+source/libidn2/2.3.7-2build1.1) |
| libssh | [0.10.6-2ubuntu0.5](https://launchpad.net/ubuntu/+source/libssh/0.10.6-2ubuntu0.5) |
| Nettle / Hogweed | [nettle 3.9.1-2.2build1.1](https://launchpad.net/ubuntu/+source/nettle/3.9.1-2.2build1.1) |
| libtasn1 | [libtasn1-6 4.19.0-3ubuntu0.24.04.2](https://launchpad.net/ubuntu/+source/libtasn1-6/4.19.0-3ubuntu0.24.04.2) |
| keyutils | [1.6.3-3build1](https://launchpad.net/ubuntu/+source/keyutils/1.6.3-3build1) |

For any bundled LGPL library, CI artifact inspection must establish the
applicable version and license alternative, the matching patched source/build
material, and whether users can actually replace the shared library or relink
the combined work. A renamed repair-tool library is not by itself proof of a
suitable shared-library mechanism. Static linkage can require application
object files or Corresponding Application Code and relinking instructions;
LGPL 3.0 can also require Installation Information. Do not prohibit reverse
engineering for debugging library modifications. Fulfill the applicable
source delivery conditions, not merely point to an upstream home page.
If a GPL-only component (for example an XZ package script) is distributed,
provide its complete corresponding source, modifications and build/install
scripts through a method allowed by the applicable GPL. Those file-scoped
terms do not turn liblzma's 0BSD license or zstd's selected BSD alternative
into GPL-only terms. Source availability and relinking remain publication
gates until the actual artifacts and the distribution arrangements are verified.

GoogleTest is fetched for test builds and is not selected by the wheel build.
Optional gRPC / Protobuf integrations are also disabled in that configuration.
Distributions that include those components must include their upstream
licenses and any notices required by their transitive dependencies.
