// Sorting stuff
// Operates on `fds::FListEntry`. The radix sort reads `entry.sortKey`
// directly (was: deref through entry.face->SortZ.DW), so each pass
// streams 16-byte contiguous slots instead of pointer-chasing into
// random Face structs.
//
// Both arrays (f and s) must hold at least n entries. The sort is
// out-of-place and leaves the sorted result in `f`; `s` ends in an
// indeterminate but written state.
//
// Histograms are stack-local so concurrent per-light shadow sorts
// don't race on them.

void Radix_SortingASM(fds::FListEntry *f, fds::FListEntry *s, unsigned int n)
{
  // FDS_NO_SORT=1 skips the radix entirely — used to bench whether the
  // rasterizer pipeline is sort-order-independent (it should be, given
  // a Z-buffer). Cached once: the flag is process-lifetime.
  static const bool sNoSort = fds::FeatureFlags::no_sort();
  if (sNoSort) return;
  if (n == 0) return;

  int32_t Bytes1[257] = {};
  int32_t Bytes2[257] = {};
  int32_t Bytes3[257] = {};
  int32_t Bytes4[257] = {};

  uint32_t Z;
  for (unsigned int I = 0; I < n; ++I) {
    Z = f[I].sortKey;
    Bytes1[ (Z & 0x000000FF)     + 1]++;
    Bytes2[((Z & 0x0000FF00) >> 8) + 1]++;
    Bytes3[((Z & 0x00FF0000) >>16) + 1]++;
    Bytes4[ (Z >> 24)            + 1]++;
  }

  for (int I = 1; I < 256; ++I) {
    Bytes1[I] += Bytes1[I-1];
    Bytes2[I] += Bytes2[I-1];
    Bytes3[I] += Bytes3[I-1];
    Bytes4[I] += Bytes4[I-1];
  }

  fds::FListEntry *F = f, *FEnd = f + n;
  for (; F < FEnd; ++F) {
    Z = Bytes1[F->sortKey & 0x000000FF]++;
    s[Z] = *F;
  }
  F = s; FEnd = s + n;
  for (; F < FEnd; ++F) {
    Z = Bytes2[(F->sortKey & 0x0000FF00) >> 8]++;
    f[Z] = *F;
  }
  F = f; FEnd = f + n;
  for (; F < FEnd; ++F) {
    Z = Bytes3[(F->sortKey & 0x00FF0000) >> 16]++;
    s[Z] = *F;
  }
  F = s; FEnd = s + n;
  for (; F < FEnd; ++F) {
    Z = Bytes4[F->sortKey >> 24]++;
    f[Z] = *F;
  }
}
