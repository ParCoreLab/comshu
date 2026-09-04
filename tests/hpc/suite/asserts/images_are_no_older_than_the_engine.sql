-- Every libpcomm object mapped into the traced process was built with the
-- engine that loaded it — or after it, never long before.
--
-- WHY THIS IS A SEPARATE CHECK FROM build_is_the_boards_commit, and not a
-- refinement of it. The build id is compiled into the ENGINE. A provider .so
-- that is hours stale, linked against that engine, reports the engine's commit
-- quite happily — so L1 sees nothing. That is not hypothetical: job 45331793
-- reported `uct_binding.accepted = 0` on a UCX version the guard had just been
-- widened to admit, from `.so` files hand-copied into a second directory while
-- the build tree beside them was correct. It nearly refuted a correct change,
-- and it is the reason `Invocation.images` exists at all.
--
-- `images` is one line per mapped libpcomm object: `<path> <size> <mtime>`,
-- walked from /proc/self/maps at emit time (engine.cpp, mapped_images).
--
-- THE THRESHOLD IS MEASURED, NOT GUESSED. On the MN5 board (job 45426015, a
-- full four-tree build) every provider is NEWER than the engine, by 1 s
-- (ibverbs, cupti — same cmake invocation), 20 s (gpumem), 29 s (ucx,
-- ucx_deep) and 35 s (nccl): the side trees are built after build/ and their
-- libraries copied in. Nothing legitimate lands older. 60 s is comfortably
-- past the intra-build spread and still catches the two-hour-stale copy above.
--
-- The direction is deliberate. A provider NEWER than the engine was built
-- against it; a provider OLDER was built against something else. The reverse
-- case — a stale ENGINE under fresh providers — is what
-- build_is_the_boards_commit catches, because the stale commit is the one
-- stamped into it. The two together cover the mixed tree; neither does alone.
--
-- NOT `-- requires: invocation`, for the reason that file gives at length: a
-- capture with no invocation row has unknown provenance, which is the state
-- these checks exist to refuse, and requiring the table would turn the defect
-- into a [skip]. This one needs no `provenance` row either — it compares the
-- capture against itself, so it is armed on a hand-run as well as on a board.
--
-- THE PATH IS PARSED FROM THE LEFT, which assumes no space in it. A build path
-- with a space would misparse into a non-numeric size, and the `?` guards below
-- would then report it as unknown rather than silently pass; sqlite has no
-- `reverse()` to parse from the right with.
WITH RECURSIVE
-- Peel `images` one line at a time. `rest` always holds what is left, with the
-- trailing newline the engine already writes; instr(rest, nl) = 1 means an
-- empty line, which is how the final newline terminates the walk.
img(rank, rest) AS (
    SELECT CAST(rank AS INTEGER), coalesce(images, '') FROM invocation
  UNION ALL
    SELECT rank, substr(rest, instr(rest, char(10)) + 1)
    FROM img WHERE instr(rest, char(10)) > 0
),
line(rank, text) AS (
    SELECT rank, substr(rest, 1, instr(rest, char(10)) - 1)
    FROM img WHERE instr(rest, char(10)) > 1
),
parts(rank, path, mtime) AS (
    SELECT rank,
           substr(text, 1, instr(text, ' ') - 1),
           -- everything after the second space: the mtime field, or '?' when
           -- the file was gone at stat() time
           substr(substr(text, instr(text, ' ') + 1),
                  instr(substr(text, instr(text, ' ') + 1), ' ') + 1)
    FROM line WHERE instr(text, ' ') > 0
),
engine(rank, mtime) AS (
    SELECT rank, CAST(mtime AS INTEGER) FROM parts
    WHERE path LIKE '%/libpcomm_engine.so' AND mtime GLOB '[0-9]*'
)

SELECT 'mapped image is older than the engine that loaded it' AS problem,
       p.rank                                                 AS rank,
       p.path                                                 AS detail,
       e.mtime - CAST(p.mtime AS INTEGER)                     AS seconds_older
FROM parts p JOIN engine e ON e.rank = p.rank
WHERE p.path NOT LIKE '%/libpcomm_engine.so'
  AND p.mtime GLOB '[0-9]*'
  AND e.mtime - CAST(p.mtime AS INTEGER) > 60

UNION ALL

-- The engine records `<path> ? ?` when a mapped object's file could not be
-- stat'd — deleted or replaced under a running job. That is not a tidiness
-- problem: it means the thing that answered cannot be identified afterwards,
-- which is the same unknown-provenance state as a missing invocation row.
SELECT 'mapped image file was gone when the capture was written',
       rank, path, NULL
FROM parts WHERE mtime = '?'

UNION ALL

-- The two vacuity companions. Empty `images` means /proc/self/maps could not
-- be read; images without the engine means the walk found providers but not
-- the object every one of them is linked against. Either way the comparison
-- above has nothing to compare and must not report that as clean.
SELECT 'rank recorded no mapped pcomm images at all',
       CAST(i.rank AS INTEGER), NULL, NULL
FROM invocation i WHERE coalesce(i.images, '') = ''

UNION ALL

SELECT 'mapped images do not include libpcomm_engine.so',
       CAST(i.rank AS INTEGER), NULL, NULL
FROM invocation i
WHERE coalesce(i.images, '') <> ''
  AND NOT EXISTS (SELECT 1 FROM engine e WHERE e.rank = CAST(i.rank AS INTEGER));
