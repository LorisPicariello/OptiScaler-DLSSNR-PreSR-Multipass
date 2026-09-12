# NR resource reclamation

The Cyberpunk run on `5089822e` reported up to 34.03 GB of VRAM use and repeated
`abandoning GPU ownership with unresolved command recordings at teardown` messages.
NR time rose from approximately 4 ms to 23 ms near the memory peak. This supports
memory pressure as a contributor to the reported long-session slowdown; it does
not establish that every allocation in the process belongs to NR.

The correction addresses three ownership gaps:

- Command lists may be destroyed instead of reset. Each NR lifetime tracker installs
  a private COM notification on recorded lists. Destruction closes their recordings
  without retaining or dereferencing the list. Submitted GPU fences still have to complete.
- Replaced NR owners remain registered for completion/reset notifications until their
  work is safe to destroy, rather than immediately abandoning the entire instance.
  NR-owned presentation lists are logically closed at retirement because they cannot
  be replayed; game-owned lists retain their reset/destruction and completion requirements.
- Codec-only NR shaders register for queue notifications too. Finished-picture capture
  commands join the parent's lifetime tracking. Once the parent is drained, discarded
  private-upscaler generations can be freed even if their GPU timestamp was never written.
  Submission hooks are installed before any feature-creation recording, not only after
  the first model becomes ready to evaluate.

Notification iteration uses a stable owner snapshot; destruction is deferred until
notifications finish. Child codecs may enqueue retirement while their parent is destroyed.
Failed completion proofs and genuinely unresolved work at process teardown remain retained
to avoid freeing memory still referenced by GPU commands.

The production lifetime WARP test covers 64 destroyed, unsubmitted command lists, destruction
of a submitted list while its GPU queue is blocked, later reclamation, replay, wrapped
identities and multiple queues. Proxy regressions and the Release x64 build passed.
In-game VRAM behaviour over repeated mode changes and an extended session requires user testing.
