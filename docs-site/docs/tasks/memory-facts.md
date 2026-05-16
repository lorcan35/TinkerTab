---
title: Memory & facts
sidebar_label: Memory & facts
---

# Memory & facts

The **Memory** skill stores things you tell Tab5. Stored as semantic embeddings (`nomic-embed-text`, 768-dim) on Dragon's SQLite. Auto-injected into every LLM context where relevant.

## Tell it something

> "Hey, remember my dog's name is Mochi."

Behind the scenes:

1. LLM sees the request, calls `<tool>remember</tool><args>{"fact":"my dog's name is Mochi","tags":["pet","name"]}</args>`
2. Memory service computes the embedding, INSERTs into the `memory_facts` table
3. LLM speaks back: *"Got it. I'll remember Mochi."*

The fact is now searchable.

## Recall it

> "What's my dog's name?"

What happens:

1. The user query is embedded
2. Memory service does cosine similarity over all stored facts
3. Top matches above a threshold are injected into the LLM's system prompt as a `<memory>` block
4. LLM answers using the recalled context: *"Mochi."*

You can also *explicitly* recall via the LLM calling `<tool>recall</tool>` (does the same search but as an explicit tool call), which is useful when the memory injection didn't catch a relevant fact.

## Browse + edit

Open the **Memory** screen from the nav sheet. You'll see all stored facts with timestamps + tags. Tap a fact to edit; long-press to delete.

You can also CRUD via the REST API:

```bash
# List
curl -H "Authorization: Bearer $TOKEN" \
     http://192.168.1.91:3502/api/v1/memory

# Add
curl -X POST -H "Authorization: Bearer $TOKEN" \
     -d '{"content":"Mochi is a cocker spaniel","tags":["pet"]}' \
     http://192.168.1.91:3502/api/v1/memory

# Search
curl -X POST -H "Authorization: Bearer $TOKEN" \
     -d '{"query":"my dog","limit":5}' \
     http://192.168.1.91:3502/api/v1/memory/search

# Delete
curl -X DELETE -H "Authorization: Bearer $TOKEN" \
     http://192.168.1.91:3502/api/v1/memory/<fact_id>
```

## Forget something (confirm-gated)

> "Forget what I told you about Bob."

The `forget` tool requires the LLM to confirm before deleting:

1. LLM searches memory for facts mentioning Bob
2. Returns a confirm prompt: *"I found 3 facts about Bob. Delete them all?"*
3. You answer yes / no
4. On yes, `forget` deletes them and acks

This is intentional — irreversible deletes shouldn't be one-shot.

## Documents

Memory also handles **long-form documents**. Ingest a document → it's chunked (512 tokens, 50 token overlap), each chunk embedded, stored with `sqlite-vec` for fast vector search.

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
     -d '{"title":"My recipe book","content":"...long text..."}' \
     http://192.168.1.91:3502/api/v1/documents
```

When the LLM is asked something where a document would help, the relevant chunks (top-k via cosine similarity) are injected into context alongside facts.

## Privacy + retention

- **Where stored**: Dragon's SQLite at `~/.tinkerclaw/dragon.db` (encrypted at rest is **not** on by default — see `SECURITY.md`)
- **What's stored**: the literal fact text + a 768-dim embedding + tags + timestamp
- **Retention**: forever (no auto-purge). Use the Memory screen or REST DELETE to remove.
- **Backups**: `cp ~/.tinkerclaw/dragon.db ~/backup-$(date +%Y%m%d).db`. The DB is the entire system state.

## When recall feels off

- **Top-k threshold** — if relevant facts aren't being injected, lower the threshold in Dragon's `config.yaml` → `memory.recall_threshold`. Default is 0.7; 0.6 is more permissive.
- **Tag filters** — if you have many similar facts, tag them at write time and the LLM can filter by tag during recall
- **Embedding model drift** — if you change the embedding model, *all* existing embeddings are stale. Re-embed by clearing memory and re-inserting, or run the migration script in `scripts/`.
