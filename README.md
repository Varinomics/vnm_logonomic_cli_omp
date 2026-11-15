# Logonomic oh-my-pi add-on

Native Logonomic CLI add-on for [oh-my-pi](https://github.com/can1357/oh-my-pi),
using the public C++20 Logonomic CLI add-on SDK.

Implementation is being committed in reviewable steps. The initial repository
bootstrap does not yet contain a runnable add-on.

## Implementation sequence

1. Package, launcher discovery, and interactive launch preparation.
2. Native protocol integration with bounded process framing and cancellation.
3. Session lifecycle, attention, and supported task capabilities with regression tests.
4. Package validation, host conformance, and documented native compatibility.

Capabilities are advertised only when their implementation is present. Provider
protocol details and source revisions will be recorded with the implementation.
