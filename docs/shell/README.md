# Shell

The current shell runs initrd commands and supports bounded native CLI parameter conversion.
The richer language below remains a proposal.

| Document | Status |
| --- | --- |
| [Build and run guide](../../README.md) | Current launcher and shell workflows |
| [Rich parameter ABI](../native-api/abi/rich-parameters-abi.md) | Implemented constructor/method inputs and dot-based CLI values |
| [Language and runtime proposal](language-proposal.md) | Future language, values, reflection, and event-dispatch model |
| [Service roadmap](../native-api/roadmap.md#other-goals-dependencies) | Dependencies for pipelines, redirection, PTYs, and supervision |

The proposed language does not define a second object ownership or authorization model.
It builds on the [Native API](../native-api/README.md).
