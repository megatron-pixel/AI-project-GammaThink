# Third-party materials and distribution

The proprietary license in [LICENSE](LICENSE) applies only to rights owned
or licensable by the GammaThink author. External code, libraries, runtimes,
model weights, datasets and knowledge archives retain their respective
owners and license terms. A public download or a dependency link does not
transfer their ownership to GammaThink.

The separate [published-knowledge license](KNOWLEDGE_LICENSE.md) applies
only to material its rights holder expressly designates. Referencing an
external source in a CoK graph or compiling a GKS snapshot does not
relicense that source or remove its notices and distribution conditions.

## Identified dependencies

`CMakeLists.txt` requires GMP/gmpxx and a crypto library for the core.
The full build additionally links libzim, libxml2, libcurl, json-c,
SQLite and Zstandard. The optional model runtime links an externally
supplied llama.cpp/ggml build and includes its dependency headers.

This list identifies build boundaries; it is not a complete license
inventory or a certification that a particular binary distribution
complies with every dependency's terms. Retain all required notices
and license texts for the exact versions actually used and comply with
their source, relinking or other distribution requirements.

## libzim: GPL distribution constraint

The current full-build configuration links libzim. Its upstream license
and the installed `zim/archive.h` header identify the GNU General Public
License, version 2 or later:

- https://github.com/openzim/libzim/blob/main/README.md#license
- https://github.com/openzim/libzim/blob/main/COPYING

GPL requirements can apply to distribution of a combined or linked work,
not just to redistribution of an unchanged dependency file. The GammaThink
license does not override them or provide an exception. Do not assume
that a full libzim-linked executable may be distributed under proprietary
restrictions without resolving license compatibility or obtaining any
necessary separate permission. This issue is not resolved merely by
linking dynamically or asking users to download a dependency separately.

The core-only configuration does not link libzim, but its own dependencies
still require review. Neither this notice nor the project license grants
permission to disregard third-party obligations.

## Models and knowledge sources

Externally obtained model weights, adapters incorporating third-party
weights, datasets and ZIM/GKS source content require their own rights
assessment. Permission to use GammaThink does not grant permission to
redistribute, sell or relicense those materials.

## Earlier project licenses

Valid irrevocable permissions already granted for earlier project copies
remain subject to their original terms. Replacing the current license does
not remove those permissions or rewrite the licensing history of existing
releases, forks or branches.
