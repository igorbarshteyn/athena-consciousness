# Licensing scope — Athena Consciousness

**Project:** `igorbarshteyn/athena-consciousness`  
**License:** Athena Noncommercial, Attribution and Responsible Deployment License 2.0  
**Reviewed pre-adoption commit:** `f779a98246d4a8cf69c2bbf6e905a1439fefd0f3`  
**Preparation date:** September 19, 2026 (UTC)

## What the new license covers

The accompanying [LICENSE](LICENSE) is offered only for the licensor-controlled, copyrightable original contributions intentionally published with this license, including future such contributions. It may coexist with earlier grants in pre-existing material; it does not extinguish those grants. The project name identifies the work, not a claim of ownership in every dependency.

This scope document accompanies an authorized adoption; preparing it locally is not itself publication. The first authorized commit supplying this scope document and the matching license is the adoption point. A permanent release record should then identify that commit. The pre-adoption hash above is an observed snapshot, **not** a fabricated first-restricted or last-permissive commit.

## Reviewed boundary

No project-wide license declaration found in the reviewed current README or root tree; historical grants and inherited material still require review.

The review covered current repository metadata, tree/README and relevant installer information, together with selected primary licensing sources. It did not audit all historical releases, privately shared archives, contributors, individual lines of source, or installed binaries. Assertions of copyright and exclusivity must remain limited to rights actually controlled. See [LICENSING-TRANSITION.md](LICENSING-TRANSITION.md).

## Path-level scope guide

| Path/category | Treatment |
|---|---|
| `patches/talk-llama/athena_*.h` and other overlay-specific additions | Authorized original expression can be covered, subject to inherited code and any earlier grants. Filenames alone are not proof of sole authorship. |
| `patches/talk-llama/talk-llama.cpp`, inherited memory code and build files | Mixed-origin stock Athena/whisper.cpp material and modifications; preserve applicable baseline and upstream rights. |
| `patches/talk-llama/mtmd/` and `patches/whisper-common/` | Embedded upstream-derived vision/audio material. Preserve applicable upstream licenses and notices; exact revision and nested-library inventory remain review tasks. |
| `patches/orpheus/`, launcher and speech wrapper | May combine prior baseline expression with new overlay contributions. Do not erase the baseline's earlier permissions. |
| Installation entry point, README and consciousness changelog | New authorized expression can be covered; historical records do not retroactively alter past grants. |
| Baseline repository, external models, GPU stack, runtime dependencies and personal memory | Not relicensed by uploading this package to the overlay repository. |

## Explicit exclusions and independent rights

Third-party works remain subject to their own terms. Earlier MIT/Apache or other valid permissions remain available. User data and ordinary model outputs are not covered merely because Athena handles or creates them. Independent implementations and unprotectable ideas, interfaces and functionality are not swept into this license by similarity.

This package does not claim a complete line-level copyright map. When preparing a distribution, preserve the more specific source notices and establish any unclear rights rather than relying on this guide as an ownership certificate. A later edited scope document cannot retroactively capture independently licensed material.

## Relationship to the other repository

Apply each repository's actual licenses and notices to the contributions it supplies. A newly restricted overlay does not make an old baseline noncommercial by itself. Conversely, the baseline's MIT history does not automatically grant MIT rights in later independently restricted overlay contributions. An installed combined tree must retain both sets of applicable notices and all upstream terms.
