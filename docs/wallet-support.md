# Wallet support in the node

A node built with the options `neutrino` and `wallet` (see
[optional-features.md](optional-features.md)) answers the wallet of its own
user about its scripts, token categories, transactions and reusable payment
address (RPA) prefixes. It needs no index and no separate server.

The answers come from the node's own chain: its stored blocks, its chain state
and its mempool. The blocks from the wallet's birthday are read once, in
parallel batches, and every block that connects afterwards is read as it
arrives. A reorganization rolls each watched target back to the fork and the
new branch is read. Nothing is asked of any peer and nothing new is stored: the
only memory is what the watched targets own and a bounded cache of blocks.

- **Histories, balances and unspent outputs** of a script, a script known by
  its SHA-256, or a token category, with the mempool's transactions apart.
- **A transaction by its hash alone**, with its Merkle branch, even one whose
  outputs are all spent: from the mempool, the watched histories, the block at
  a height hint, and then the stored blocks read from the tip down.
- **RPA**: the transactions with one of their first 30 inputs in a prefix,
  confirmed in a range of heights or in the mempool.
- **Broadcast** to the node's own mempool, which relays it.

The calls are `kth::node::wallet::service`, made with `service::from_chain`.

## From peers

A node that keeps only headers answers the same calls from its peers, with
`service::from_peers`. It validates the headers as a kth node does and keeps,
in two flat files per filter type, the filters of every block from the
wallet's birthday:

- the **regular filter** (filter type 0, served by kth with
  `node.compact_filters` and by bchd): every output script of a block and
  every outpoint it spends;
- the **input prefix filter** (filter type 2), when `input_prefix_filters` is
  set: the first two bytes of the double SHA-256 of each of the first 30
  inputs of each transaction after the coinbase.

Both are built from the block alone. Peers are asked for `getcfcheckpt`,
`getcfheaders` and `getcfilters` of these two types only. Every peer that
serves a type gives its checkpoints; lists that differ are settled by the
block where the filter headers part, and above the last checkpoint the stored
filter headers are compared with another peer's. Each downloaded block
rebuilds both filters and compares them with what was served: a peer whose
filter does not fit its block is refused, and what was read from that filter
is read again.

- **Histories, balances and unspent outputs** of a script: the regular filters
  point to the blocks that pay it, and to the blocks that spend an output it
  owns, by that output's outpoint. Only those blocks are downloaded, each with
  three other blocks of its bucket of 144 heights (the same ones in every
  session) and each over a connection of its own.
- **A transaction by its hash** when it is in a watched history, in the
  mempool copy, or given with its height.
- **An output by its outpoint** when its transaction is known: the regular
  filters point to the blocks that may spend it.
- **RPA**: with input prefix filters, the blocks whose filter holds a value
  with the prefix, each with its decoys; without them, every block of the
  range.
- **The mempool copy** from ordinary relay: every announced transaction is
  fetched, so what is asked for names nothing watched.

The regular filter holds output scripts as they are and no transaction
hashes, so from peers a script known only by its hash, a token category, and
a transaction known by its hash alone are answered `not_implemented`. Those
wait for a filter that holds script keys, category keys and transaction keys.

A browser node (the option `browser_node`) built with `neutrino` and `wallet`
answers from peers the same way: its peers are ws:// or wss:// bridges, it runs
on the Worker's runtime, which header clients share, and scans inline.
