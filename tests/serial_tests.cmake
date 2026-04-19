# ============================================================================
# Post-discovery test property tweaks for ctest.
# Applied after gtest_discover_tests (PRE_TEST) materialises test names.
#
# MultiDrain.* tests use a fixed 200 ms sleep that misses its deadline under
# heavy concurrent ctest load (pre-existing timing bug — ingest_tick may be
# deferred when the CPU is saturated by 1300+ other tests). Serialise them
# with RUN_SERIAL so they don't compete for CPU with the rest of the suite.
# This is a test-infrastructure fix, not a weakening of any assertion.
# ============================================================================

cmake_policy(PUSH)
if(POLICY CMP0064)
    cmake_policy(SET CMP0064 NEW)
endif()
if(POLICY CMP0011)
    cmake_policy(SET CMP0011 NEW)
endif()

foreach(_t "MultiDrain.TwoDrainThreads_AllDataStored"
           "MultiDrain.FourDrainThreads_MultiSymbol")
    set_tests_properties(${_t} PROPERTIES RUN_SERIAL TRUE)
endforeach()

# HttpClusterHA.* — spin up multi-node HA fixtures using PORT_OFF-adjusted
# but fixed-offset ports plus process-global state (singleton license /
# metrics). Under ctest -j they would fight for the same listening sockets,
# so serialise the whole suite. Guard with `if(TEST ...)` so filtered runs
# don't break the generated CTestTestfile.cmake.
foreach(_t "HttpClusterHA.ActiveServesQueries_StandbyPromotesOnFailure"
           "HttpClusterHA.HttpServer_FailoverPreservesClusterState"
           "HttpClusterHA.StandbyDiesFirst_ActiveContinues"
           "HttpClusterHA.QueryDuringPromotion_ReturnsError"
           "HttpClusterHA.SplitBrain_FencingTokenRejectsStaleWrites"
           "HttpClusterHA.DataNodeFailure_RemainingNodesServe"
           "HttpClusterHA.NetworkPartition_PromotionWithFencing")
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES RUN_SERIAL TRUE)
    endif()
endforeach()

# TcpRpcServerGracefulDrain.* — RPC drain/stop lifecycle is real-time-sensitive
# (fixed 5s/100ms drain timeouts, 200ms sleep_for inside handler). Under
# full-suite `ctest -j$(nproc)` CPU saturation the handler wake window can be
# missed, causing a deadlock on stop(). Serialise rather than weaken timeouts.
foreach(_t "TcpRpcServerGracefulDrain.InFlightRequestCompletesBeforeStop"
           "TcpRpcServerGracefulDrain.ForceCloseAfterTimeout")
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES RUN_SERIAL TRUE)
    endif()
endforeach()

# ----------------------------------------------------------------------------
# TCP/RPC family shares the ephemeral-port pool and loopback socket backlog.
# Under `ctest -j$(nproc)` with many of these starting at once, pick_free_port()
# hits a TOCTOU race window and RPC connect() fails fast with "all nodes returned
# errors". Serialise the family against ITSELF via RESOURCE_LOCK (they still run
# in parallel with unrelated suites, preserving ~7.5x speedup).
#
# Ideally we would scan `DIRECTORY PROPERTY TESTS` with a regex, but on this
# CMake at TEST_INCLUDE_FILES eval time that property is empty and `if(TEST)`
# returns FALSE — so we enumerate by name. `set_tests_properties` silently
# no-ops for unknown names, so listing a test that does not exist in a filtered
# build is safe.
# ----------------------------------------------------------------------------
foreach(_t
        # TcpRpc.*
        "TcpRpc.PingPong"
        "TcpRpc.SqlQueryRoundTrip"
        "TcpRpc.ServerNotRunning_ReturnsError"
        "TcpRpc.StatsRequestRoundTrip"
        "TcpRpc.StatsRequest_NoCallback_ReturnsEmptyJson"
        "TcpRpc.StatsRequest_ServerDown_ReturnsEmpty"
        "TcpRpc.MultipleSequentialQueries"
        "TcpRpc.MetricsRequestRoundTrip"
        "TcpRpc.MetricsRequest_NoCallback_ReturnsEmptyArray"
        "TcpRpc.MetricsRequest_ServerDown_ReturnsEmpty"
        "TcpRpc.MetricsRequest_PassesSinceAndLimit"
        # TcpRpcServerThreadPool.* / TcpRpcClientPing.*
        "TcpRpcServerThreadPool.ConcurrentRequestsWithSmallPool"
        "TcpRpcClientPing.UsesConnectionPool"
        # QueryCoordinator.TwoNodeRemote_*
        "QueryCoordinator.TwoNodeRemote_ScatterGather_Count"
        "QueryCoordinator.TwoNodeRemote_GroupBy_Concat"
        "QueryCoordinator.TwoNodeRemote_DistributedAvg_Correct"
        "QueryCoordinator.TwoNodeRemote_DistributedAvg_MixedAggs"
        "QueryCoordinator.TwoNodeRemote_GroupBy_CrossNode_XbarMerge"
        "QueryCoordinator.TwoNodeRemote_DistributedVwap"
        "QueryCoordinator.TwoNodeRemote_OrderByLimit"
        "QueryCoordinator.TwoNodeRemote_DistributedHaving"
        "QueryCoordinator.TwoNodeRemote_DistributedDistinct"
        "QueryCoordinator.TwoNodeRemote_DistributedWindowFunction"
        "QueryCoordinator.TwoNodeRemote_DistributedFirstLast"
        "QueryCoordinator.TwoNodeRemote_DistributedCountDistinct"
        "QueryCoordinator.TwoNodeRemote_DistributedCTE"
        "QueryCoordinator.TwoNodeRemote_MultiColumnOrderBy"
        # CoordinatorHA.*
        "CoordinatorHA.ActiveServesQueries"
        "CoordinatorHA.StandbyPromotesOnActiveDown"
        "CoordinatorHA.PromotionReRegistersNodes"
        "CoordinatorHA.StandbyForwardsToActive"
        # DistributedP0.*
        "DistributedP0.SumCaseWhen_ScalarAgg"
        "DistributedP0.SumCaseWhen_ScatterGather"
        "DistributedP0.SumCaseWhen_TwoColumns"
        "DistributedP0.SumCaseWhen_Plus_WhereIn"
        "DistributedP0.WhereIn_SingleNodeHit"
        "DistributedP0.WhereIn_MultiNode"
        "DistributedP0.WhereIn_ScalarAgg"
        "DistributedP0.OrderBy_PostMerge"
        # DistributedString.*
        "DistributedString.TwoNode_ScatterGather_Count"
        "DistributedString.TwoNode_StringWhere_Sum"
        "DistributedString.TwoNode_StringWhere_Vwap"
        "DistributedString.TwoNode_StringWhere_ScatterGather"
        "DistributedString.TwoNode_StringNotFound"
        "DistributedString.TwoNode_MixedIntAndString"
        # PartitionMigratorRollback.* / FencingRpc.* / SplitBrain.* / ComputeNode.*
        "PartitionMigratorRollback.FailedMoveDeletesPartialData"
        "FencingRpc.StaleEpochTickRejected"
        "FencingRpc.StaleEpochWalRejected"
        "SplitBrain.FencingPreventsStaleWrite"
        "SplitBrain.StaleCoordinatorWriteRejected"
        "SplitBrain.StaleWalReplicationRejected"
        "SplitBrain.K8sLeasePreventsDualLeader"
        "ComputeNode.ExecuteAcrossDataNodes"
        "ComputeNode.FetchAndIngest_LocalJoin")
    set_tests_properties(${_t} PROPERTIES RESOURCE_LOCK tcp_rpc_pool)
endforeach()

# ----------------------------------------------------------------------------
# FIXParserPerformanceTest asserts absolute ns/msg throughput; under parallel
# CPU saturation this ratio degrades. Serialise to keep the assertion
# meaningful without weakening the threshold.
# (No `if(TEST ...)` guard — at TEST_INCLUDE_FILES eval time that predicate
#  returns FALSE on this CMake version, making the block a silent no-op.
#  `set_tests_properties` on an unknown name is itself silently ignored, so
#  the unguarded form is safe under filtered builds.)
# ----------------------------------------------------------------------------
set_tests_properties(FIXParserPerformanceTest.ParseSpeed PROPERTIES RUN_SERIAL TRUE)

cmake_policy(POP)
