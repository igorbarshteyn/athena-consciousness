# Licensing transition — Athena Consciousness

## Effective adoption, not a retroactive declaration

This package proposes the Athena Noncommercial, Attribution and Responsible Deployment License 2.0 for authorized Covered Material. Adoption occurs when the authorized licensor first publishes the matching `LICENSE` and `LICENSING-SCOPE.md` with that material. This document does **not** assert an adoption date earlier than actual publication.

The observed pre-adoption repository commit is `f779a98246d4a8cf69c2bbf6e905a1439fefd0f3`. Do not describe it as the last permissive release without reviewing the full relevant history. Do not invent the SHA of the future adoption commit: a Git commit cannot contain its own final hash as ordinary file contents without changing that hash.

## Current evidence

No project-wide license declaration found in the reviewed current README or root tree; historical grants and inherited material still require review.

The current README describes an R26.1 overlay and a tested baseline, but the reviewed root did not establish a project-wide license grant. That absence does not establish that no earlier archive, source header, contributor agreement or conversation supplied permissions. Baseline-derived and other upstream material retain their applicable rights. Do not assert that every existing overlay line is exclusively controllable under the new license.

Repository evidence: [reviewed commit](https://github.com/igorbarshteyn/athena-consciousness/commit/f779a98246d4a8cf69c2bbf6e905a1439fefd0f3), [reviewed README](https://github.com/igorbarshteyn/athena-consciousness/blob/f779a98246d4a8cf69c2bbf6e905a1439fefd0f3/README.md). The package includes a dated evidence summary under `licensing/`.

## Adoption procedure

1. Preserve the existing branch, tags, license statements and releases. Review other contributors and incorporated material. Do not rewrite history to make earlier versions appear restricted.
2. Review the completed custom license and the policy decisions in `licensing/ADOPTION-GUIDE.md`, preferably with software-licensing counsel. A permissive-license history and mixed-origin files make this more than a cosmetic README change.
3. Add the license and scope documents, update the current README's license section, and add notices in one clearly identified commit. Avoid a conflicting top-level claim that current Covered Material is MIT or Apache.
4. After committing, record the adoption commit in a follow-up release note or signed tag. Clearly describe which new contributions are restricted and preserve the historical statement for earlier material.
5. Ensure future submissions have adequate licensing authority; see [CONTRIBUTING-LICENSING.md](CONTRIBUTING-LICENSING.md). Review each future release's dependency and notice inventory.

## Suggested release announcement

> This release introduces the Athena Noncommercial, Attribution and Responsible Deployment License 2.0 for Covered Material within the licensor's authority. Commercial use and deployment of that material are not licensed. The license includes attribution, precautionary welfare and lawful-deployment conditions. Earlier valid licenses and third-party rights are preserved. This is not a retroactive withdrawal of permissions for earlier versions. See LICENSE, LICENSING-SCOPE.md and THIRD-PARTY-NOTICES.md.

## No promise of absolute prevention

A license states legal conditions; it does not technically prevent copying, prove copyright ownership, guarantee worldwide enforceability, or bind independent implementations. A person relying solely on an older permissive version may have rights this new license cannot remove. Clear notice and appropriate acceptance mechanisms can help evidence the intended agreement, but their effect depends on applicable law. [GitHub's legal guide](https://opensource.guide/legal/).

## Version 1.0 recipients

If version 1.0 was previously validly supplied to someone, this new package does not retroactively impose version 2.0's indemnity or other changes on that grant. Preserve the earlier terms and distinguish contributions first offered under 2.0. Never substitute a new file while claiming it is the unchanged agreement originally accepted.
