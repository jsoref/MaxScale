/**
 * @file regexfilter1.cpp Simple regexfilter tests; also regression case for mxs508 ("regex filter ignores
 * username")
 *
 * Three services are configured with regexfilter, each with different parameters.
 * All services are queried with SELECT 123. The first service should replace it
 * with SELECT 0 and the second and third services should not replace it.
 */


#include <iostream>
#include <maxtest/testconnections.hh>

int main(int argc, char* argv[])
{
    TestConnections* test = new TestConnections(argc, argv);
    test->maxscale->connect_maxscale();
    test->add_result(execute_query_check_one(test->maxscale->conn_rwsplit, "SELECT 123", "0"),
                     "Query to first service should have replaced the query.\n");
    test->add_result(execute_query_check_one(test->maxscale->conn_slave, "SELECT 123", "123"),
                     "Query to second service should not have replaced the query.\n");
    test->add_result(execute_query_check_one(test->maxscale->conn_master, "SELECT 123", "123"),
                     "Query to third service should not have replaced the query.\n");
    test->maxscale->close_maxscale_connections();
    int rval = test->global_result;
    delete test;
    return rval;
}
