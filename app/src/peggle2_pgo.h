// Peggle 2: profile collection of the PGO-instrumented Switch build (PEGGLE2_PGO=generate), see peggle2_pgo.cpp.
// In every other build these do nothing. From sa2-nx (sa2_pgo.h).

#pragma once

namespace peggle2::pgo {

// Picks this session's file (pgo/peggle2_pgo_<N>.stream next to the executable) and starts the 2-minute dumps.
void Start();
// Writes every count so far to this session's file (the whole session each time, replacing the previous dump).
void DumpNow();
// The last dump, then the dump thread ends.
void Stop();

}  // namespace peggle2::pgo
