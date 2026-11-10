#include <gtest/gtest.h>

#include <cstring>

#include "discovery/completion.hpp"
using namespace os1;
namespace
{
discovery::Grants grants()
{
    discovery::Grants g{};
    g.root.kind = OS1_OBJECT_DIRECTORY;
    g.root.rights = OS1_RIGHT_ALL;
    g.ceiling = OS1_RIGHT_ALL;
    g.shell = true;
    g.streams[1].kind = OS1_OBJECT_CONSOLE;
    g.streams[1].rights = OS1_RIGHT_WRITE | OS1_RIGHT_INSPECT;
    return g;
}
struct Discovery : testing::Test
{
    context::Data data{};
    discovery::View view;
    void SetUp() override
    {
        context::defaults(data);
        ASSERT_TRUE(view.build(grants(), data, 1));
    }
};
TEST_F(Discovery, LiveRightsExcludeUnavailableServicesAndStreamMethods)
{
    int input = view.resolve("os.input");
    ASSERT_GE(input, 0);
    EXPECT_EQ(OS1_HARDWARE_INPUT_ROOT, view.nodes[input].target);
    EXPECT_EQ(discovery::Alias, view.nodes[input].flags);
    EXPECT_GE(view.resolve("os.system.log.print"), 0);
    EXPECT_GE(view.resolve("process.streams.output.write"), 0);
    EXPECT_LT(view.resolve("process.streams.output.try_read"), 0);
    EXPECT_LT(view.resolve("os.devices"), 0);
    auto g = grants();
    g.root.rights = OS1_RIGHT_LOOKUP;
    g.ceiling = OS1_RIGHT_READ;
    ASSERT_TRUE(view.build(g, data, 2));
    EXPECT_LT(view.resolve("os.input"), 0);
    EXPECT_LT(view.resolve("os.system"), 0);
    EXPECT_LT(view.resolve("fs.create_file"), 0);
    EXPECT_GE(view.resolve("fs.copy"), 0);
    EXPECT_LT(view.resolve("process.streams.input"), 0);
    EXPECT_LT(view.resolve("fs.cwd"), 0);
    g.root = {};
    ASSERT_TRUE(view.build(g, data, 3));
    EXPECT_GE(view.resolve("path.list"), 0);
    EXPECT_LT(view.resolve("path.search"), 0);
    EXPECT_LT(view.resolve("os.fs"), 0);
}
TEST_F(Discovery, EnvironmentListsNestedFieldsAndAliasesAreStructural)
{
    auto record = context::add(data, 0, "config", 6, OS1_VALUE_RECORD);
    context::add(data, record, "enabled", 7, OS1_VALUE_BOOLEAN, nullptr, 0, 1);
    ASSERT_TRUE(view.build(grants(), data, 2));
    int n = view.resolve("process.environment.config.enabled");
    ASSERT_GE(n, 0);
    EXPECT_EQ(OS1_VALUE_BOOLEAN, view.nodes[n].type);
    EXPECT_EQ(1, view.environment.nodes[view.nodes[n].value].integer);
    EXPECT_GE(view.resolve("process.environment.path.1"), 0);
    EXPECT_EQ(view.resolve("fs.copy"), view.resolve("os.fs.copy"));
    EXPECT_EQ(view.resolve("out.print"), view.resolve("process.streams.output.print"));
    EXPECT_LT(view.resolve("os..fs"), 0);
    EXPECT_LT(view.resolve("os.fs."), 0);
}
TEST_F(Discovery, PagesAreBoundedAndRefreshRejectsOldCursor)
{
    discovery::Cursor cursor{};
    discovery::Page page{};
    int fs = view.resolve("fs");
    ASSERT_EQ(0, view.enumerate(fs, cursor, page));
    EXPECT_EQ(discovery::page_limit, page.count);
    EXPECT_FALSE(page.end);
    unsigned last = page.ids[page.count - 1];
    ASSERT_EQ(0, view.enumerate(fs, cursor, page));
    EXPECT_GT(page.ids[0], last);
    ASSERT_TRUE(view.build(grants(), data, 2));
    EXPECT_EQ(OS1_ERR_CANCELLED, view.enumerate(fs, cursor, page));
    cursor = {};
    cursor.next = discovery::node_limit + 1;
    EXPECT_EQ(OS1_ERR_INVALID, view.enumerate(fs, cursor, page));
}
TEST_F(Discovery, RejectsHostileNamesAndCycles)
{
    auto bad = data;
    bad.text[bad.nodes[1].name] = 27;
    EXPECT_FALSE(view.build(grants(), bad, 2));
    ASSERT_TRUE(view.build(grants(), data, 3));
    int alias = view.resolve("fs");
    view.nodes[alias].target = alias;
    EXPECT_LT(view.resolve("fs.copy"), 0);
}
TEST_F(Discovery, CompletionDrillsAndAcceptsWithoutExecution)
{
    discovery::Completion c;
    ASSERT_TRUE(c.begin(view));
    EXPECT_STREQ("os", view.nodes[c.ids[c.selected]].name);
    ASSERT_TRUE(c.accept(view, true));
    EXPECT_STREQ("os.", c.line);
    EXPECT_TRUE(c.open);
    c = {};
    for(char k : "os.system.log.pr")
        if(k)
            c.insert(k);
    ASSERT_TRUE(c.begin(view));
    ASSERT_EQ(1, c.matches);
    ASSERT_TRUE(c.accept(view, false));
    EXPECT_STREQ("os.system.log.print after:unset limit:20 level:info", c.line);
    EXPECT_FALSE(c.open);
    EXPECT_FALSE(c.detail);
    EXPECT_GE(c.focus, 0);
    EXPECT_EQ(3u, c.field_count);
    EXPECT_FALSE(c.detail);
    c.cancel();
}
TEST_F(Discovery, CompletionLimitsQuotingAndStaleSelection)
{
    discovery::Completion c;
    for(char k : "echo \"os.")
        if(k)
            c.insert(k);
    EXPECT_FALSE(c.begin(view));
    c = {};
    for(char k : "echo process.environment.c")
        if(k)
            c.insert(k);
    ASSERT_TRUE(c.begin(view));
    ASSERT_TRUE(c.accept(view, false));
    EXPECT_STREQ("echo process.environment.cwd", c.line);
    c = {};
    ASSERT_TRUE(c.begin(view));
    ASSERT_TRUE(view.build(grants(), data, 2));
    EXPECT_FALSE(c.accept(view, false));
    for(unsigned i = 0; i < os1::shell::line_capacity - 1; ++i)
        ASSERT_TRUE(c.insert('x'));
    EXPECT_FALSE(c.insert('x'));
    EXPECT_EQ(os1::shell::line_capacity - 1, c.size);
}
TEST_F(Discovery, MaximumEnvironmentFitsWithoutTruncatingMembers)
{
    context::defaults(data);
    for(unsigned i = data.count; i < OS1_CONTEXT_NODES; ++i)
    {
        char name[] = {'n', char('a' + i / 26), char('a' + i % 26), 0};
        ASSERT_GE(context::add(data, 0, name, 3, OS1_VALUE_INTEGER, nullptr, 0, i), 0);
    }
    auto g = grants();
    for(auto& s : g.streams)
    {
        s.kind = OS1_OBJECT_CONSOLE;
        s.rights = OS1_RIGHT_ALL;
    }
    ASSERT_TRUE(view.build(g, data, 1));
    EXPECT_LT(view.count, discovery::node_limit);
}
}  // namespace
TEST_F(Discovery, IndexedPathsAndCompletionUseCompactPositions)
{
    unsigned collection = view.add(view.resolve("os"), "pointers");
    unsigned item = view.add(collection, "0");
    unsigned field = view.add(item, "name", discovery::Kind::Field);
    EXPECT_EQ(int(field), view.resolve("os.pointers[0].name"));
    EXPECT_LT(view.resolve("os.pointers[-1]"), 0);
    EXPECT_LT(view.resolve("os.pointers[]"), 0);
    EXPECT_LT(view.resolve("os.pointers[0]name"), 0);
    char path[256]{};
    ASSERT_TRUE(discovery::member_path(view, field, path, sizeof(path)));
    EXPECT_STREQ("os.pointers[0].name", path);
    discovery::Completion c;
    for(char ch : std::string("os.pointers."))
        ASSERT_TRUE(c.insert(ch));
    ASSERT_TRUE(c.begin(view));
    ASSERT_TRUE(c.accept(view, true));
    EXPECT_STREQ("os.pointers[0].", c.line);
    ASSERT_TRUE(c.begin(view));
    ASSERT_TRUE(c.accept(view, false));
    EXPECT_STREQ("os.pointers[0].name", c.line);
}
