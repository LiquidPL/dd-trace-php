extern "C" {
#include "symbols/symbols.h"
}

#include "tea/testing/catch2.hpp"
#include <cstdlib>
#include <cstring>

TEA_TEST_CASE("symbol/lookup/function", "global, exists", {
    zai_string_view lower = ZAI_STRL_VIEW("strlen");
    zai_string_view mixed = ZAI_STRL_VIEW("strLen");

    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_GLOBAL, NULL, &lower));
    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_GLOBAL, NULL, &mixed));
})

TEA_TEST_CASE("symbol/lookup/function", "global, does not exist", {
    zai_string_view lower = ZAI_STRL_VIEW("nosuchfunction");
    zai_string_view mixed = ZAI_STRL_VIEW("NoSuchFunction");

    REQUIRE(!zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_GLOBAL, NULL, &lower));
    REQUIRE(!zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_GLOBAL, NULL, &mixed));
})

TEA_TEST_CASE("symbol/lookup/function", "root ns, exists", {
    zai_string_view ns   = ZAI_STRL_VIEW("\\");
    zai_string_view lower = ZAI_STRL_VIEW("strlen");
    zai_string_view mixed = ZAI_STRL_VIEW("strLen");

    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_NAMESPACE, &ns, &lower));
    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_NAMESPACE, &ns, &mixed));
})

TEA_TEST_CASE("symbol/lookup/function", "root ns fqcn, exists", {
    zai_string_view name = ZAI_STRL_VIEW("\\strlen");

    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_GLOBAL, NULL, &name));
})

TEA_TEST_CASE("symbol/lookup/function", "root ns, does not exist", {
    zai_string_view ns   = ZAI_STRL_VIEW("\\");
    zai_string_view lower = ZAI_STRL_VIEW("nosuchfunction");
    zai_string_view mixed = ZAI_STRL_VIEW("NoSuchFunction");

    REQUIRE(!zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_NAMESPACE, &ns, &lower));
    REQUIRE(!zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_NAMESPACE, &ns, &mixed));
})

TEA_TEST_CASE_WITH_STUB("symbol/lookup/function", "ns, exists", "./stubs/lookup/function/Stub.php", {
    zai_string_view ns   = ZAI_STRL_VIEW("\\DDTraceTesting");
    zai_string_view name = ZAI_STRL_VIEW("StubFunction");

    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_NAMESPACE, &ns, &name));
})

TEA_TEST_CASE_WITH_STUB("symbol/lookup/function", "ns fqcn, exists", "./stubs/lookup/function/Stub.php", {
    zai_string_view name = ZAI_STRL_VIEW("\\DDTraceTesting\\StubFunction");

    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_GLOBAL, NULL, &name));
})

TEA_TEST_CASE_WITH_STUB("symbol/lookup/function", "class method exists, public", "./stubs/lookup/function/Stub.php", {
    zai_string_view scope = ZAI_STRL_VIEW("\\DDTraceTesting\\Stub");
    zai_string_view method = ZAI_STRL_VIEW("existsPublic");

    zend_class_entry *ce = (zend_class_entry*) zai_symbol_lookup(ZAI_SYMBOL_TYPE_CLASS, ZAI_SYMBOL_SCOPE_GLOBAL, NULL, &scope);

    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_CLASS, ce, &method));
})

TEA_TEST_CASE_WITH_STUB("symbol/lookup/function", "class method exists, protected", "./stubs/lookup/function/Stub.php", {
    zai_string_view scope = ZAI_STRL_VIEW("\\DDTraceTesting\\Stub");
    zai_string_view method = ZAI_STRL_VIEW("existsProtected");

    zend_class_entry *ce = (zend_class_entry*) zai_symbol_lookup(ZAI_SYMBOL_TYPE_CLASS, ZAI_SYMBOL_SCOPE_GLOBAL, NULL, &scope);

    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_CLASS, ce, &method));
})

TEA_TEST_CASE_WITH_STUB("symbol/lookup/function", "class method exists, private", "./stubs/lookup/function/Stub.php", {
    zai_string_view scope = ZAI_STRL_VIEW("\\DDTraceTesting\\Stub");
    zai_string_view method = ZAI_STRL_VIEW("existsPrivate");

    zend_class_entry *ce = (zend_class_entry*) zai_symbol_lookup(ZAI_SYMBOL_TYPE_CLASS, ZAI_SYMBOL_SCOPE_GLOBAL, NULL, &scope);

    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_CLASS, ce, &method));
})

TEA_TEST_CASE_WITH_STUB("symbol/lookup/function", "object method exists, public", "./stubs/lookup/function/Stub.php", {
    zai_string_view scope = ZAI_STRL_VIEW("\\DDTraceTesting\\Stub");
    zai_string_view method = ZAI_STRL_VIEW("existsPublic");

    zval object;

    zend_class_entry *ce = (zend_class_entry*) zai_symbol_lookup(ZAI_SYMBOL_TYPE_CLASS, ZAI_SYMBOL_SCOPE_GLOBAL, NULL, &scope);

    zai_symbol_new(&object, ce, 0);

    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_OBJECT, &object, &method));

    zval_ptr_dtor(&object);
})

TEA_TEST_CASE_WITH_STUB("symbol/lookup/function", "object method exists, protected", "./stubs/lookup/function/Stub.php", {
    zai_string_view scope = ZAI_STRL_VIEW("\\DDTraceTesting\\Stub");
    zai_string_view method = ZAI_STRL_VIEW("existsProtected");

    zval object;

    zend_class_entry *ce = (zend_class_entry*) zai_symbol_lookup(ZAI_SYMBOL_TYPE_CLASS, ZAI_SYMBOL_SCOPE_GLOBAL, NULL, &scope);

    zai_symbol_new(&object, ce, 0);

    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_OBJECT, &object, &method));

    zval_ptr_dtor(&object);
})

TEA_TEST_CASE_WITH_STUB("symbol/lookup/function", "object method exists, private", "./stubs/lookup/function/Stub.php", {
    zai_string_view scope = ZAI_STRL_VIEW("\\DDTraceTesting\\Stub");
    zai_string_view method = ZAI_STRL_VIEW("existsPrivate");

    zval object;

    zend_class_entry *ce = (zend_class_entry*) zai_symbol_lookup(ZAI_SYMBOL_TYPE_CLASS, ZAI_SYMBOL_SCOPE_GLOBAL, NULL, &scope);

    zai_symbol_new(&object, ce, 0);

    REQUIRE(zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_OBJECT, &object, &method));

    zval_ptr_dtor(&object);
})

TEA_TEST_CASE_WITH_TAGS("symbol/lookup/function", "incorrect API usage", "[use][.]", {
    REQUIRE(!zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_STATIC, NULL, NULL));
    REQUIRE(!zai_symbol_lookup(ZAI_SYMBOL_TYPE_FUNCTION, ZAI_SYMBOL_SCOPE_FRAME, NULL, NULL));
})
