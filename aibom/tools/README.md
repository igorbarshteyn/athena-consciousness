# AIBOM utilities

These tools support the documentation package. They do not change Athena's runtime.

`validate_aibom.py` requires Python 3.10+ and `jsonschema>=4.18,<5`. The delivered package was validated with jsonschema 4.26.0. It resolves schemas locally and checks identifiers, graph relationships, evidence hashes, optional exact source checkouts and the package checksum manifest. Revision 3 also checks the projector pin/size/hash across records, build-only acquisition flows, absence of invented installation observations, 49 unchanged licensing snapshot files, exact embedded license text, 16 scoped component declarations, 15 obligation mappings, historical MIT evidence, and the separation of current sources from the original licensing review and unverified adoption/assent. It exits nonzero on failure.

The checks validate this specific revision-3 documentary profile. They are not an installer integration test, license compatibility engine, counsel approval, proof of copyright title, or detector of actual contract formation. Recording new source pins, installation observations, adoption/assent evidence or another license version requires a deliberate AIBOM/profile revision, not removing validation checks to force a pass. The original supplied license package's README mutation scripts are not included or executed.

Run `python3 -B aibom/tools/test_aibom.py` in the same validation environment for 16 offline synthetic tests. They exercise evidence/license tampering, accidental whole-model relicensing, misleading MIT alternatives, invented adoption, unsupported implementation claims, preservation of historical MIT evidence, projector identity/flow/observation boundaries, original licensing-review identity and collector privacy/overwrite behavior. They mutate temporary copies only and do not run Athena or make network calls. These are AIBOM tests, not the 34 local installer tests or live publisher test reported in the overlay README; the README explicitly excludes that development tooling, and its results were not independently reproduced here.

`collect_deployment.py` needs only Python 3.10+ and optional Git. It reads named files and writes a **new private JSON file outside the checkouts**. It never runs the installed binaries or reads memory, logs, audio or image content. Model paths are reference candidates until checked against the actual launch configuration.

From the repository root:

```bash
python3 aibom/tools/collect_deployment.py \
  --athena /absolute/path/to/athena \
  --overlay /absolute/path/to/athena-consciousness \
  --output /private/location/athena-deployment-2026-09-19.json
```

Add `--hash-models` to stream the listed model files through SHA-256. This reads approximately 179 GB for the reference Qwen shards alone, plus other models; it does not load models into an inference engine. Native binaries are small enough to hash by default.

For a customized path, supply `--model M-projector=/path/to/projector.gguf`, for example. That replaces all default paths for the specified role. Repeat `--model M-qwen=...` once per shard if replacing the main split model. Overrides are not silently matched to a different model's registry hash.

The collector does not infer that present files are loaded, that a Git checkout produced a binary, or that inherited environment values reached a desktop session. Add sanitized active configuration, startup observations, library/package inventories and live acceptance evidence separately. Keep the output private; file paths and hashes can themselves reveal deployment details.

The collector refuses to overwrite an existing output file and creates new output with mode 0600. Its output is not part of the public release AIBOM unless deliberately reviewed/redacted and incorporated as a new document revision.
