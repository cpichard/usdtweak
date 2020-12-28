#pragma once
#include <array>
///
/// Constants used in multiple files
///

/// Size of the main windows when it opens.
constexpr int InitialWindowWidth = 1024;
constexpr int InitialWindowHeight = 1024;

/// Buffer size of a TextEdit widget,
constexpr size_t TextEditBufferSize = 256;

/// Viewport border between the panel and the window
constexpr float ViewportBorderSize = 60.f;

/// Max buffer size for file path, used in file browser
constexpr size_t PathBufferSize = 1024;
constexpr float AuthoredAttributeColor4[4] = { 1.0, 1.0, 1.0, 1.0 };
constexpr std::array<float, 4> AuthoredAttributeColor2{ 1.0, 1.0, 1.0, 1.0 };
///
#define AttributeAuthoredColor {1.0, 1.0, 1.0, 1.0}
#define AttributeUnauthoredColor {0.5, 0.5, 0.5, 1.0}
#define AttributeRelationshipColor {0.5, 0.5, 0.9, 1.0}
#define AttributeConnectionColor {0.5, 0.5, 0.9, 1.0}

/// Decimal Precision shown in the floating point values UI
constexpr size_t DecimalPrecision = 5;

/// Name
constexpr const char * const DefaultPrimSpecName = "prim";

/// Return error codes
constexpr int ERROR_UNABLE_TO_COMPILE_SHADER = 110;
