# Upload the Athena R26.1 AIBOM — revision 3

1. Extract this ZIP.
2. Open [igorbarshteyn/athena-consciousness](https://github.com/igorbarshteyn/athena-consciousness), choose **Add file -> Upload files**, and upload `AIBOM.md`, `UPLOAD.md` and the entire `aibom` folder at the repository root. Preserve the folder structure. Do not upload the ZIP itself as a substitute for the browsable files.
3. If an earlier AIBOM revision is already present, replace its AIBOM files with revision 3, including the new `aibom/source-update.json`. Preserve unrelated repository changes. Review the diff and commit, for example with the message `Update Athena R26.1 AIBOM for pinned vision projector installation`.

Optionally add this link to the existing README:

```markdown
[Athena R26.1 AI Bill of Materials](AIBOM.md)
```

The package adds documentation, JSON evidence, schemas and validation/collection tools, plus unmodified licensing document snapshots. It contains no Athena runtime replacement files, model weights, personal memory or executed acceptance records. The supplied license-package README mutation scripts are not included. No GitHub changes have been made by preparing this package.

The current source inventory describes overlay commit `b3c6e54ead4976c043cd0fca9452bd07d7e2689c` and unchanged stock commit `e747c0362c4e1c3b95756a57154d09b8df7811ce`. Revision 3 records the pinned projector installer and retains the supplied `LicenseRef-Athena-NCARD-2.0` evidence. The licensing package's earlier overlay review commit, `f779a98246d4a8cf69c2bbf6e905a1439fefd0f3`, remains a separate historical identifier. None is asserted to be a license-adoption commit. A documentation-only upload creates a new commit without changing the recorded source snapshot. Record any intervening application changes and revise affected entries rather than silently treating the new HEAD as the analyzed code.

## Keep AIBOM publication separate from license adoption

`aibom/licensing/snapshot/athena-upload/` and `aibom/licensing/snapshot/athena-consciousness-upload/` are documentary evidence, **not** instructions to move those directories to this repository's root. Do not replace root `LICENSE`, `NOTICE`, `README.md` or licensing policy files merely to publish this AIBOM. Use the separately supplied complete licensing package's matching upload tree and adoption procedure for each repository. The stock notice must not be substituted for the overlay notice.

The snapshots exclude the original mutation scripts and their checksum manifests; obtain those from the original licensing package if needed. Included acceptance/deployment forms are blank templates, not evidence of assent or compliance. Keep completed forms and personal details out of GitHub.

If you adopt or have already adopted the licenses, record the **real adoption SHA for each repository** in a follow-up AIBOM revision, reconcile historical grants and scope, and revalidate/rehash. A commit cannot accurately embed its own future hash. Publishing the nested snapshots or this AIBOM alone is not verified adoption, acceptance, notice propagation or runtime enforcement. Earlier valid grants and upstream terms remain separate.

The main machine-readable file is `aibom/athena-r26.1.cdx.json` (CycloneDX 1.7). The directed-flow companion has its own clearly labeled project schema. GitHub can store and display these files, but committing them does not automatically import model or software entries into the dependency graph.

This is a **partial reference AIBOM**, ready to publish with its disclosed limitations. It is not an approved production-deployment attestation. Supplied first-party license text is now documented; adoption, historical ownership/grants, exact model rights, assent, installed-artifact identity and deployed safeguards remain distinct verification items. See `AIBOM.md` and `aibom/license-mapping.json`.

To verify unchanged package bytes from the extracted directory:

```bash
sha256sum -c aibom/SHA256SUMS
```

For schema and cross-reference validation, use the commands in `AIBOM.md`. Keep private deployment-collector output outside the Git checkout. Update checksums only after intentional document revisions and successful validation.
