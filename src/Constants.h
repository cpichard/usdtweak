#pragma once
///
/// Constants used in multiple files
///

/// Size of the main windows when it opens.
constexpr int InitialWindowWidth = 640;
constexpr int InitialWindowHeight = 480;

/// Buffer size of a TextEdit widget,
constexpr size_t TextEditBufferSize = 256;

/// Viewport border between the panel and the window
constexpr float ViewportBorderSize = 60.f;

/// Max buffer size for file path, used in file browser
constexpr size_t PathBufferSize = 1024;


/// Decimal Precision shown in the floating point values UI
constexpr size_t DecimalPrecision = 5;

/// Name
constexpr const char * const DefaultPrimSpecName = "prim";
