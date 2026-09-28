# lob::engine - price-time priority limit order book (header-heavy; the hot path is inline).
add_library(lob_engine src/order_book.cpp)
add_library(lob::engine ALIAS lob_engine)
set_target_properties(lob_engine PROPERTIES EXPORT_NAME engine)
target_include_directories(lob_engine PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(lob_engine PUBLIC cxx_std_20)
lob_target_defaults(lob_engine)
