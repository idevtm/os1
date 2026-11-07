# Shell

The current shell provides typed context, navigation, aliases, basic file operations, redirection, and a line editor.
The richer language below remains a proposal.

| Document | Status |
| --- | --- |
| [Typed context and basic commands](typed-context.md) | Implemented first stage, startup v3, rooted navigation, and same-filesystem moves |
| [First-stage implementation record](typed-context-plan.md) | Plan, impact, checks, resource comparison, and review |
| [Interactive files and editor](interactive-files.md) | Implemented commands, redirection, EOF, and limits |
| [Implementation evidence](../archive/shell/2026-11-06-interactive-files.md) | Plan, measurements, checks, and independent review |
| [Build and run guide](../../README.md) | Current launcher and shell workflows |
| [Rich parameter ABI](../native-api/abi/rich-parameters-abi.md) | Implemented constructor/method inputs and dot-based CLI values |
| [Object commands and short bindings](object-commands.md) | Settled command direction: aliases, copy policies, mixed arguments, interactive text fallback, and strict scripts; first stage implemented |
| [File utilities and filesystem extensions](file-utilities.md) | Accepted minimal utilities and delivery stages; first stage implemented; later tools, permissions, and links deferred |
| [Language and runtime proposal](language-proposal.md) | Future language, values, reflection, and event-dispatch model |
| [Service roadmap](../native-api/roadmap.md#other-goals-dependencies) | Dependencies for pipelines, redirection, PTYs, and supervision |

The proposed language does not define a second object ownership or authorization model.
It builds on the [Native API](../native-api/README.md).
