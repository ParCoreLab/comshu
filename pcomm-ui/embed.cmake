# Turn index.html into a C++ header holding it as a raw string literal.
#
# A raw string with a delimiter the page cannot contain: `)pcommhtml"` is not a
# sequence any HTML or JS in there will produce, whereas the bare `)"` of an
# undelimited raw string absolutely is (`fn()"` appears in ordinary JS).
file(READ "${IN}" _html)
file(WRITE "${OUT}"
"// GENERATED from pcomm-ui/index.html — do not edit, do not commit.
#pragma once
namespace pcomm::ui {
inline constexpr const char* kIndexHtml = R\"pcommhtml(${_html})pcommhtml\";
}  // namespace pcomm::ui
")
