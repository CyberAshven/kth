---
name: "knuth-review-practices"
description: "Review rules for changes to the Knuth BCH node (k-nuth/kth): CodeRabbit, test ports and readiness, strictness, coverage, branch handling, proving fixes and PR text. Use before opening, updating or replying on a kth PR."
---

# Knuth (kth) review practices

These rules come from maintainer review on k-nuth/kth. A request made on one PR applies to every open PR and to every branch not yet opened, and is checked and fixed there before it has to be asked again.

## Review rules

1. **CodeRabbit comments are resolved before approval is requested.**
   - Reviews are triggered by the maintainer, never from our side (rate limits; owner's decision, 2026-11-01). See the `coderabbit-review` skill.
   - Each finding is checked against the current code. Suggested fixes can be wrong: on #770 the removal of a parser flag was suggested, but the feature depends on that flag, so the gap was closed another way.
   - Valid findings are fixed and invalid ones are answered with evidence. Every thread ends resolved.

2. **Test servers bind an OS-assigned port and report readiness.**
   - Port 0 is bound, and the test waits until the component reports the port it listens on, or reports that it stopped.
   - Fixed ports are not used: a fixed port can reach an unrelated listener even when the server under test failed to bind. A successful connect is not proof that the server is up.

3. **A failing test fails; it does not hang.**
   - A missing response fails an assertion instead of waiting for the CI timeout, and every wait is bounded. For example, the client's send side is shut down after writing.

4. **Leniency stays limited to its goal.**
   - When a parser, rule or default is relaxed for one client, everything else stays as strict as before. Example: bare-LF request lines were allowed while lone-CR request lines stayed rejected.
   - Edge inputs are compared with BCHN's behaviour on the same input; BCHN 29.2.0 answers that lone-CR request with 400.
   - Negative tests cover what must still be refused, including input split across reads and mixed framing.

5. **The coverage gate passes.**
   - `codecov/patch` fails below the project target (82.69% on 2026-11-01). New error branches are covered before pushing, for example with a stored block file damaged on purpose to reach "cannot read" and "cannot parse".

6. **PR branches can be changed by the maintainer.**
   - Reworks are force-pushed to PR branches; #769 and #770 were reworked this way on 2026-11-01.
   - `git fetch` runs before every push, and a push is made only when the branch has not moved. A moved branch is built on or answered with a reply, never pushed over.
   - Replies name only commits that are on the branch.

7. **CI on an external PR waits for maintainer approval** (#800, 2026-11-01).
   - Every push to an open PR costs the maintainer an approval before its CI runs.
   - Fixes are proven in `kth-rows.yml` first and batched into one push per PR. New PRs are opened a few at a time, not all at once.

## How changes are made

- **Shape.** One issue per PR, with fix and tests together, in kth's style: Catch2 cases next to the existing tests, and comments that explain why. CI is not changed inside a feature PR.
- **Base.** A branch starts from `upstream/master`, or is stacked on the PR it builds on, with "This builds on #N" in the text.
- **Master moving into a PR's files.** `upstream/master` is merged into the branch, not rebased, so no force-push is needed. Conflicts are resolved by whole marker lines only; a `=======` at the end of a `// ====` comment rule once fooled a regex resolver.
- **Proof before push.** The fix goes to a `test/...` branch on the fork and is built and tested with kth's recipe through the package repo's `kth-rows.yml` (`knuth-rpc-e2e` branch). The client's end-to-end result from `scripts/knuth_rpc_e2e.py` is checked too. Only then is the PR branch fast-forwarded.
- **After a push.** Each review thread gets a reply with only what changed and the result: commit, test names, suite counts.

## PR and review text

- The text describes the behaviour and the client that needs it.
- A change is never framed as "match BCHN", and never as helping P2Pool v2.
- No "happy to", no closing process lines, no questions to the maintainer; changes are proposed.
- A change in what existing users see is stated plainly, in bold.
- Private conversations with the maintainer are never mentioned.
