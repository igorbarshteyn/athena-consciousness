# Schema provenance and format mapping

The unmodified CycloneDX 1.7 validation schemas come from the [official specification repository](https://github.com/CycloneDX/specification/tree/4b3f59453366e27c8073fd24e98bf21ef8892c8e/schema), tag `1.7`, resolved commit `4b3f59453366e27c8073fd24e98bf21ef8892c8e`:

- `bom-1.7.schema.json`
- `spdx.schema.json`
- `jsf-0.82.schema.json`

Their Apache-2.0 license is reproduced in `CycloneDX-LICENSE.txt`. Original schema comments and embedded third-party attribution are retained. Package SHA-256 values identify these exact bytes.

`athena-graph-1.0.schema.json` describes this package's **project-specific companion JSON**. It is not part of CycloneDX and is not an official OWASP schema. Its URL-shaped identifier is a stable identifier; validation uses the local file and does not assume that URL is a hosted schema endpoint.

CycloneDX 1.7 owns inventory, model-card data, services, dependencies and composition. Its standard `properties` fields carry names beginning `athena:`; that prefix is local and is not asserted to be registered in the CycloneDX property taxonomy. Compound property values are serialized JSON strings.

The graph companion adds explicit directed flows, logical zones/boundaries, conditional behavior, lifecycle distinctions, lineage references and evidence gaps. Every graph node binds to a CycloneDX component/service `bom-ref`. The reference validator checks these bindings, including both directions of boundary-to-flow membership. Dependency edges must not be interpreted as data-flow edges.

The supplied Foundations Guide anticipates CycloneDX 2.0 blueprints. Until a stable 2.0 release/schema is adopted, this companion preserves that information without inserting unsupported fields into a document labeled 1.7. Migration should preserve stable identities and the distinction between declared and observed claims.
