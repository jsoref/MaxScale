#include <maxtest/testconnections.hh>
#include <maxtest/maxrest.hh>
#include <sstream>

size_t get_size(TestConnections& test)
{
    auto res = test.maxctrl("api get maxscale/threads/0 data.attributes.stats.query_classifier_cache.size");
    int64_t size = 0;
    std::istringstream ss(res.output);
    ss >> size;
    return size;
}

int main(int argc, char** argv)
{
    TestConnections test(argc, argv);

    size_t size = get_size(test);
    test.tprintf("Initial cache size: %lu", size);
    test.expect(size == 0, "Expected an empty cache, got %lu bytes", size);

    auto c = test.maxscale->rwsplit();
    c.connect();
    c.query("SELECT 1");    // This query should end up in the cache

    size = get_size(test);
    test.tprintf("Cache size after one query: %lu", size);
    test.expect(size != 0, "Expected a non-empty cache");

    int QUERIES = 20;

    for (int i = 0; i < QUERIES && test.ok(); i++)
    {
        c.query("SELECT 1");
        size_t current_size = get_size(test);
        test.expect(current_size == size, "Expected cache to be %lu bytes, not %lu", size, current_size);
    }

    size = get_size(test);
    test.tprintf("Cache size after %d queries: %lu", QUERIES + 1, size);

    return test.global_result;
}