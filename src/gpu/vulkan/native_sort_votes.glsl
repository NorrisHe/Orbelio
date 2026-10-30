shared uint votes[radix * maskWords];
void buildVotes(bool valid, uint digit) {
    uint local = gl_LocalInvocationIndex;
    for (uint i = local; i < radix*maskWords; i += groupSize) votes[i] = 0;
    barrier();
    if (valid) atomicOr(votes[digit*maskWords + local/32], 1u << (local%32));
    barrier();
}
uint stableRank(uint digit) {
    uint local = gl_LocalInvocationIndex;
    uint word = local / 32;
    uint rank = bitCount(votes[digit*maskWords+word] & ((1u << (local%32))-1u));
    for (uint i = 0; i < word; ++i) rank += bitCount(votes[digit*maskWords+i]);
    return rank;
}
