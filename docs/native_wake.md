# Native mailbox wake

`session-wake@1` uses OMP's public custom-message `aside` delivery to notify the
exact live interactive conversation of committed Logonomic mailbox activity.
The instruction is host-authored input with Logonomic attribution. It is passed
as a hidden, agent-attributed custom message, never through command parsing or
the terminal editor.

## Native admission and policy

The bridge calls `pi.sendMessage(message, {deliverAs: "aside"})`. In the recorded
OMP source, an aside starts a turn when idle even without `triggerTurn`. Native
plan mode and deliberate user interruption fold it into context without an
autonomous turn. A race with another turn queues the aside at a step boundary
without interrupting the current tool batch. OMP retains its own model, account,
cost preflight, and session-generation admission checks.

The public API returns `void`. A submitted notification therefore receives
`uncertain` with `native_admission_unacknowledged`. This means submission may have
succeeded, and the host must not retry it automatically. It does not assert a
started turn, a mailbox read, or recipient acknowledgment. Those facts retain
their existing native lifecycle and coordination evidence.

Before submission, the bridge checks the admitted root activation, the live
native session ID, and public session state. Known question or approval waits,
busy work, deliberate interruption, and plan/goal/loop pauses produce `deferred`
without a native call. Missing public mode-query support, a lost lifecycle
binding, or a noninteractive context makes wake unavailable. These checks use
capabilities and current native state, with no tested-version cutoff. Native
policy remains authoritative across a race after the check.

## Private launch binding

Preparation allocates a fresh owner-only registration below the SDK-provided
package profile directory. On Windows, it inherits the host profile directory's
user ACL and rejects reparse objects. The host's opaque live binding contains a
registration ID and directory, while a separate protected file retains the
random credential. A new launch or resume needs a new registration.

The root extension exclusively claims that registration and opens a loopback
listener. An inherited child process or a second CLI cannot take over the same
claim. Each request must authenticate the registration and exact native instance,
then match both the admitted provider session ID and activation nonce. A clear
that preserves OMP's native UUID still changes the nonce and retires old requests.
Inspect is read-only; notify repeats all checks before dispatch.

The listener accepts one bounded JSON request per connection, limits concurrent
connections, and closes idle peers. The adapter has a one-second total I/O
deadline and checks invocation cancellation while waiting. Any lost response or
cancellation after dispatch is uncertain. No protocol retry can create a second
native submission: the live activation remembers up to 256 dispatched
notification IDs, and then refuses further notifications until a new activation.
Deferred attempts are not recorded because they have no submission side effect.

Shutdown immediately revokes readiness, closes the listener and connections, and
removes the exact registration files. An abnormal process exit can leave an inert
registration directory, but its exclusive claim cannot be adopted by a later
process. A failed or unavailable wake preparation leaves ordinary launch,
initial prompt, lifecycle, and mailbox delivery available.

This is package-private native transport. The SDK still owns all communication
with Logonomic, including authenticated helper ingress. Endpoint credentials,
notification content, and registry data are not lifecycle observations or general
diagnostics. No host endpoint, control token, keystroke injection, session reload,
or synthetic human approval is used.

## Evidence

Native behavior is traced to upstream OMP commit
`7a52f1e35eacc115b067c03a9c8ec7263e114cda`:

- `extensibility/extensions/types.ts`: `sendMessage`, `ExtensionSessionModes`,
  `ExtensionContext.isIdle`, and the documented idle `aside` behavior.
- `session/agent-session.ts`: `#dispatchCustomMessage`,
  `#promptAgentInitiatedMessage`, and `#sessionGenerationChanged`, including
  interruption/plan handling and asynchronous session fencing.

Contract tests exercise the production preparation and C++ native transport
against the production extension listener, with native API calls observed by a
fixture. They verify exact binding, read-only inspection, deferred policies,
deduplication, private storage, revocation, and uncertainty after cancellation.
Actual same-process native idle wake is a separate PTY acceptance gate; its
result must be reported independently of these contract fixtures.
