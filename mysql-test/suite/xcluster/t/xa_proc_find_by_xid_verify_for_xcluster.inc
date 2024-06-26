
--let $save_expect_gcn= $expect_gcn
--let $save_expect_state= $expect_state
--let $save_xid= $my_xid

--connection conn1
# --source include/wait_follower_catchup.inc
select ROLE from information_schema.ALISQL_CLUSTER_LOCAL;
--let $expect_gcn= $save_expect_gcn
--let $expect_state= $save_expect_state
--let $my_xid= $save_xid
--source suite/innodb_gcn/include/xa_proc_find_by_xid_verify.inc

--connection conn2
select ROLE from information_schema.ALISQL_CLUSTER_LOCAL;
--source include/wait_apply_catchup.inc
--let $expect_gcn= $save_expect_gcn
--let $expect_state= $save_expect_state
--let $my_xid= $save_xid
--source suite/innodb_gcn/include/xa_proc_find_by_xid_verify.inc

--connection conn3
select ROLE from information_schema.ALISQL_CLUSTER_LOCAL;
--source include/wait_apply_catchup.inc
--let $expect_gcn= $save_expect_gcn
--let $expect_state= $save_expect_state
--let $my_xid= $save_xid
--source suite/innodb_gcn/include/xa_proc_find_by_xid_verify.inc
