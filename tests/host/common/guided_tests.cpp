#include <gtest/gtest.h>

#include <string>

#include "discovery/completion.hpp"
#include "shell/expression.hpp"
using namespace os1;
namespace
{
struct Guided : testing::Test
{
    discovery::View view;
    discovery::Completion editor;
    void SetUp() override
    {
        context::Data data{};
        context::defaults(data);
        discovery::Grants grants{};
        grants.root.kind = OS1_OBJECT_DIRECTORY;
        grants.root.rights = OS1_RIGHT_ALL;
        grants.ceiling = OS1_RIGHT_ALL;
        grants.shell = true;
        for(auto& stream : grants.streams)
        {
            stream.kind = OS1_OBJECT_CONSOLE;
            stream.rights = OS1_RIGHT_ALL;
        }
        ASSERT_TRUE(view.build(grants, data, 1));
    }
    void type(const char* text)
    {
        while(*text)
            ASSERT_TRUE(editor.insert(*text++));
    }
};
TEST_F(Guided, EveryExposedMethodHasExecutableParameterDefinitions)
{
    for(unsigned i = 1; i < view.count; ++i)
        if(view.nodes[i].kind == discovery::Kind::Method)
            EXPECT_TRUE(discovery::definition(view, i).known) << view.nodes[i].name;
}
TEST_F(Guided, MethodSelectionExpandsDefaultsAndRequiredFields)
{
    type("fs.cop");
    ASSERT_TRUE(editor.begin(view));
    ASSERT_TRUE(editor.accept(view, true));
    EXPECT_STREQ("fs.copy source:<?> destination:process.streams.output mode:unset recursive:false",
                 editor.line);
    EXPECT_FALSE(editor.open);
    ASSERT_GE(editor.focus, 0);
    EXPECT_EQ(editor.fields[0].begin, editor.cursor);
    type("\"a\"");
    editor.scan_fields(view);
    EXPECT_STREQ(
        "fs.copy source:\"a\" destination:process.streams.output mode:unset recursive:false",
        editor.line);
    ASSERT_TRUE(editor.step_field(view, 1));
    EXPECT_EQ(1, editor.focus);
    ASSERT_TRUE(editor.step_field(view, -1));
    EXPECT_EQ(0, editor.focus);
}
TEST_F(Guided, CursorInsertionDeletionAndFieldBoundaries)
{
    type("abcd");
    editor.horizontal(-1);
    editor.horizontal(-1);
    type("X");
    EXPECT_STREQ("abXcd", editor.line);
    editor.erase();
    EXPECT_STREQ("abXd", editor.line);
    editor.backspace();
    EXPECT_STREQ("abd", editor.line);
    editor = {};
    type("fs.change_directory path:\"/tmp\"");
    editor.scan_fields(view);
    editor.focus_field(0);
    editor.horizontal(-1, true);
    editor.backspace();
    EXPECT_STREQ("fs.change_directory path:\"/tmp\"", editor.line);
    editor.erase();
    EXPECT_STREQ("fs.change_directory path:/tmp\"", editor.line);
}
TEST_F(Guided, NestedCompletionPreservesLaterArguments)
{
    type("out.print text:path.sear suffix:3");
    editor.cursor = 24;
    ASSERT_TRUE(editor.begin(view));
    ASSERT_TRUE(editor.accept(view, true));
    EXPECT_STREQ("out.print text:path.search(path:\"<?>\") suffix:3", editor.line);
    ASSERT_GE(editor.focus, 0);
    EXPECT_GE(editor.fields[editor.focus].begin, 26u);
}
TEST_F(Guided, EnumsAndVariantCasesUseDeclaredChoices)
{
    type("fs.copy source:\"a\" destination:\"b\" mode:unset recursive:false");
    editor.scan_fields(view);
    editor.focus_field(2);
    ASSERT_TRUE(editor.value_choices(view));
    EXPECT_STREQ("unset", editor.choices[0]);
    editor.selected = 2;
    ASSERT_TRUE(editor.accept(view, true));
    EXPECT_NE(std::string::npos, std::string(editor.line).find("mode:append"));
    editor.focus_field(0);
    ASSERT_TRUE(editor.value_choices(view));
    editor.selected = 0;
    ASSERT_TRUE(editor.accept(view, true));
    EXPECT_NE(std::string::npos, std::string(editor.line).find("source.path:\"\""));
    ASSERT_GE(editor.focus, 0);
    type("/tmp/a");
    editor.scan_fields(view);
    EXPECT_NE(std::string::npos, std::string(editor.line).find("source.path:\"/tmp/a\""));
}
TEST(Expression, NestedCallsRecordsAndSourceArgumentOrder)
{
    shell::expression::Tree tree;
    ASSERT_TRUE(tree.parse("out.print text:path.search(path:process.environment.program)"));
    auto root = tree.nodes[tree.root];
    ASSERT_EQ(shell::expression::Kind::Call, root.kind);
    auto argument = tree.nodes[root.first];
    EXPECT_EQ(shell::expression::Kind::Call, argument.kind);
    EXPECT_EQ(4u, argument.names);
    ASSERT_TRUE(tree.parse("service.run options:{first:other.read(), second:[1, true, \"x\"]}"));
    EXPECT_EQ(shell::expression::Kind::Record, tree.nodes[tree.nodes[tree.root].first].kind);
    ASSERT_TRUE(tree.parse("service.info().field"));
    EXPECT_EQ(shell::expression::Kind::Access, tree.nodes[tree.root].kind);
}
TEST(Expression, MalformedSyntaxAndBudgetsFailBeforeEvaluation)
{
    for(const char* text :
        {"f(a:1", "f(a:1,)", "f(a:1) garbage", "f(options:{x:})", "f(\"bad)", "f()."})
    {
        shell::expression::Tree tree;
        EXPECT_FALSE(tree.parse(text)) << text;
    }
    shell::expression::Tree tree;
    std::string input = "f()";
    for(unsigned i = 0; i < 10; ++i)
        input = "f(" + input + ")";
    EXPECT_FALSE(tree.parse(input.c_str()));
    EXPECT_FALSE(tree.parse(std::string(1024, 'a').c_str()));
}
TEST(Expression, SerializationPreservesStringsAndStructure)
{
    context::Data value{};
    context::init(value);
    int record = context::add(value, 0, "value", 5, OS1_VALUE_RECORD);
    context::add(value, record, "text", 4, OS1_VALUE_STRING, "a\"b\n", 4);
    context::add(value, record, "number", 6, OS1_VALUE_INTEGER, nullptr, 0, -7);
    shell::expression::Text text;
    ASSERT_TRUE(text.value(value, record));
    EXPECT_STREQ("{text:\"a\\\"b\\n\",number:-7}", text.bytes);
    shell::expression::Tree tree;
    EXPECT_TRUE(tree.parse(text.bytes, false));
}
TEST_F(Guided, QuotesSuppressDiscoveryAndReturnMetadataDoesNotRequireInvocation)
{
    type("out.print text:\"process.\"");
    editor.cursor = 24;
    EXPECT_FALSE(editor.begin(view));
    editor = {};
    type("process.streams.output.geometry().");
    ASSERT_TRUE(editor.begin(view));
    EXPECT_EQ(2u, editor.matches);
    EXPECT_STREQ("columns", view.nodes[editor.ids[0]].name);
}
TEST_F(Guided, RecordFieldsAndFilteredEnumChoices)
{
    type("process.streams.input.acquire_input options:{handoff:false}");
    editor.scan_fields(view);
    ASSERT_EQ(2u, editor.field_count);
    editor.focus_field(1);
    ASSERT_TRUE(editor.value_choices(view));
    EXPECT_STREQ("true", editor.choices[1]);
    editor.selected = 1;
    ASSERT_TRUE(editor.accept(view, true));
    EXPECT_NE(std::string::npos, std::string(editor.line).find("handoff:true"));
    editor = {};
    type("fs.copy source:\"a\" mode:unset");
    editor.scan_fields(view);
    editor.focus_field(1);
    ASSERT_TRUE(editor.value_choices(view));
    type("a");
    editor.scan_fields(view);
    editor.refine(view, true);
    ASSERT_EQ(1u, editor.matches);
    EXPECT_STREQ("append", editor.choices[0]);
}
TEST_F(Guided, PagerUsesTheActualPathParameter)
{
    type("pager.sho");
    ASSERT_TRUE(editor.begin(view));
    ASSERT_TRUE(editor.accept(view, true));
    EXPECT_STREQ("pager.show path:\"<?>\"", editor.line);
}

TEST_F(Guided, CompletingEditedMethodPreservesExistingFlatArguments)
{
    type("fs.cop source:\"a\" destination:\"b\"");
    editor.cursor = 6;
    ASSERT_TRUE(editor.begin(view));
    ASSERT_TRUE(editor.accept(view, true));
    EXPECT_STREQ("fs.copy source:\"a\" destination:\"b\"", editor.line);
    editor = {};
    type("chosen = path.sear");
    ASSERT_TRUE(editor.begin(view));
    ASSERT_TRUE(editor.accept(view, true));
    ASSERT_GE(editor.focus, 0);
    EXPECT_STREQ("path", editor.field_parameter(view, editor.focus).name);
}

TEST_F(Guided, StreamVariantDefaultsFollowSourceAndDestinationRoles)
{
    type("fs.copy source:\"a\" destination:\"b\"");
    editor.scan_fields(view);
    editor.focus_field(0);
    ASSERT_TRUE(editor.value_choices(view));
    editor.selected = 1;
    ASSERT_TRUE(editor.accept(view, true));
    EXPECT_NE(std::string::npos,
              std::string(editor.line).find("source.stream:process.streams.input"));
    editor.focus_field(1);
    ASSERT_TRUE(editor.value_choices(view));
    editor.selected = 1;
    ASSERT_TRUE(editor.accept(view, true));
    EXPECT_NE(std::string::npos,
              std::string(editor.line).find("destination.stream:process.streams.output"));
}

TEST_F(Guided, UnrepresentableMemberPathReturnsAnEmptyDefinition)
{
    unsigned parent = 0;
    for(unsigned i = 0; i < 9; ++i)
        parent = view.add(parent, "long_member_name_for_path_limit");
    char path[discovery::path_limit]{};
    ASSERT_FALSE(discovery::member_path(view, parent, path, sizeof(path)));
    auto definition = discovery::definition(view, parent);
    EXPECT_FALSE(definition.known);
    EXPECT_FALSE(definition.exclusive);
    EXPECT_EQ(0u, definition.count);
}

}  // namespace
