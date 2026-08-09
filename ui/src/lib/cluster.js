// Agglomerative clustering, average linkage.
//
// Hand-written rather than pulled from a library: it is ~40 lines, the input is
// a rank×rank matrix we already have, and the alternative is a dependency whose
// only job is this. `vega-hierarchy` can *lay out* a tree but does not build
// one from a distance matrix.
//
// What it is for: the dendrogram panel asks whether ranks that talk to each
// other cluster together. On a ring the answer is a known shape, which is what
// makes it a test rather than a picture.

/**
 * Cosine distance between two rank vectors.
 *
 * Cosine, not Euclidean: a rank that moves 1.3 GB and a rank that moves 158 KB
 * to the *same* peers have the same communication *pattern*, and Euclidean
 * would call them far apart purely because of volume. The question this panel
 * asks is "who talks to whom", not "who talks most" — that is the rank panel.
 */
function cosineDistance(a, b) {
  let dot = 0, na = 0, nb = 0;
  for (let i = 0; i < a.length; i++) {
    dot += a[i] * b[i];
    na += a[i] * a[i];
    nb += b[i] * b[i];
  }
  if (na === 0 || nb === 0) return 1;
  return 1 - dot / (Math.sqrt(na) * Math.sqrt(nb));
}

/**
 * @param {number[][]} vectors  one row per leaf
 * @param {string[]}   labels   leaf labels, same order
 * @returns {{label?: string, children?: object[], height: number, leaves: number}} tree root
 */
export function cluster(vectors, labels) {
  let nodes = vectors.map((v, i) => ({
    label: labels[i], vec: v, height: 0, leaves: 1, members: [i],
  }));
  if (!nodes.length) return null;

  // Pairwise distances, recomputed against members on merge (average linkage
  // over the original vectors — simpler to read than Lance-Williams updates and
  // fast enough: rank counts are in the tens, not the thousands).
  const d = (x, y) => {
    let sum = 0;
    for (const i of x.members) for (const j of y.members) sum += cosineDistance(vectors[i], vectors[j]);
    return sum / (x.members.length * y.members.length);
  };

  while (nodes.length > 1) {
    let best = Infinity, bi = 0, bj = 1;
    for (let i = 0; i < nodes.length; i++)
      for (let j = i + 1; j < nodes.length; j++) {
        const dist = d(nodes[i], nodes[j]);
        if (dist < best) { best = dist; bi = i; bj = j; }
      }
    const a = nodes[bi], b = nodes[bj];
    const merged = {
      children: [a, b],
      height: best,
      leaves: a.leaves + b.leaves,
      members: [...a.members, ...b.members],
    };
    nodes = nodes.filter((_, k) => k !== bi && k !== bj);
    nodes.push(merged);
  }
  return nodes[0];
}

/** Leaves in draw order, left to right. */
export function leavesOf(node, out = []) {
  if (!node) return out;
  if (!node.children) out.push(node);
  else node.children.forEach((c) => leavesOf(c, out));
  return out;
}
