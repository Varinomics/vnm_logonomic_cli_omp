# Working in this repository

Read and follow the four canonical
[Varinomics standards](https://github.com/Varinomics/varinomics-standards):

- `varinomics_coding_style_guideline.md`
- `varinomics_coding_style_llm_addendum.md`
- `varinomics_review_scope.md`
- `varinomics_change_governance.md`

The add-on owns oh-my-pi's native argument and event semantics. Use the
Logonomic CLI add-on SDK for host contracts, framing, process custody and
package assembly. Advertise capabilities only with their complete native
implementation and contract tests. Native CLI calls and scripted fixtures
provide different evidence; describe that distinction in verification reports.

Owned source dependencies follow `master`. `LOGONOMIC_CLI_ADDON_SDK_SOURCE`
selects a current local SDK source tree for coordinated work. Keep builds and
transient notes outside the source checkout. Use the workstation build queue.
Hosted CI in this private repository requires an explicit human request.
