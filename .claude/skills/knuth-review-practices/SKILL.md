---
name: "knuth-review-practices"
description: "Best practices for any change to the Knuth BCH node (k-nuth/kth): the maintainer's own review rules (CodeRabbit, test ports, strictness, coverage), how to prove and push fixes, and how to write PR text. Use before opening, updating or replying on a kth PR."
---

# Knuth (kth) review practices

These come from the maintainer's (Fernando Pelliccioni's) own review comments. Treat each one as a rule for every kth PR, already open or not yet opened: when he asks for something on one PR, check every other PR for the same thing and fix it there too, before he has to ask.

## The maintainer's rules

1. **Resolve CodeRabbit before asking for approval.**
   He said: "Please resolve the CodeRabbit review comments before requesting approval." He triggers `@coderabbitai review` himself.
   - Verify every finding against the code. CodeRabbit's suggested fix can be wrong. On #770 it suggested dropping a parser flag that the feature needs; the right fix kept the flag and closed the gap another way.
   - Fix what is valid, or explain with evidence why not.
   - Reply on the thread and get it resolved.

2. **Test servers use an OS-assigned port and wait for readiness.**
   He said: "Could we use an OS-assigned port and wait for this server to report readiness? Connecting to the fixed port can hit an unrelated listener even if our server failed to bind."
   - Bind port 0 and wait until the component reports the port it actually listens on, or reports that it stopped.
   - Never test a hard-coded port, and never treat a successful connect as proof that our server is up.

3. **A failing test fails; it never hangs.**
   A missing response must fail an assertion, not wait for the CI timeout. For example, shut down the client's send side after writing, and bound every wait.

4. **Leniency must not widen what is accepted beyond its goal.**
   He said: "Please keep lone-CR request lines rejected while allowing LF, and add a negative test for the mixed framing."
   - When relaxing a parser, a rule or a default for one client, keep everything else as strict as it was.
   - Compare edge inputs against BCHN's behaviour on the same input. He checked that BCHN 29.2.0 answers such a request with 400.
   - Add negative tests for what must still be refused, including input split across reads and other boundary cases.

5. **Coverage gate.**
   kth's `codecov/patch` check fails below the project target (82.69% on 2026-11-01). Cover new error branches with tests before pushing. For example, use a stored block file damaged on purpose to reach "cannot read" and "cannot parse".

6. **He edits PR branches himself.**
   On 2026-11-01 he force-pushed his own reworks of #769 and #770 within an hour of our pushes.
   - Before every push, `git fetch` and check the PR branch has not moved since we last saw it.
   - Never push over his commits. If the branch moved, build on his version or reply instead.
   - Only name commits in a reply that are actually on the branch.

## How changes are made

- **Shape.** One issue per PR, with fix and tests together, in kth's own style: Catch2 test cases next to the code's existing tests, and comments that explain why. No CI changes inside a feature PR.
- **Base.** Branch from `upstream/master`, or stack on the PR a change builds on and say "This builds on #N" in the text.
- **When master moves into a PR's files,** merge `upstream/master` into the branch instead of rebasing, so there is no force-push. Resolve conflicts by whole git marker lines only: a `=======` at the end of a `// ====` comment rule fooled a regex resolver once.
- **Prove before pushing.**
  - Push the fix to a `test/...` branch on the fork.
  - Build and test it with kth's recipe in CI, using the package repo's `kth-rows.yml`, the test-only `knuth-rpc-e2e` branch.
  - Check the client's own end-to-end result, from `scripts/knuth_rpc_e2e.py`.
  - Only then fast-forward the PR branch.
- **After pushing,** reply on each review thread with only what changed and the result (commit, test names, suite counts). Re-run the CodeRabbit check if the maintainer asks.

## Writing PR and review text

- Describe the behaviour and the client that needs it.
- Never frame a change as "match BCHN".
- Never say a change helps P2Pool v2.
- No "happy to", no closing process lines, no questions to the maintainer; propose instead.
- When a change alters what existing users see, say so plainly in bold.
- Never mention private conversations with the maintainer.
