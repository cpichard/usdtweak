#pragma once
///
/// Constants used in the application
///

/// PI
constexpr float PI_F = 3.14159265;

/// Size of the main window when it opens.
constexpr int InitialWindowWidth = 1024;
constexpr int InitialWindowHeight = 1024;

/// Viewport border between the panel and the window
constexpr float ViewportBorderSize = 60.f;
/// Height of a row in the property editor
constexpr float TableRowHeight = 22.f;

/// Predefined colors for the different widgets
#define AttributeAuthoredColor {1.0, 1.0, 1.0, 1.0}
#define AttributeUnauthoredColor {0.5, 0.5, 0.5, 1.0}
#define AttributeRelationshipColor {0.5, 0.5, 0.9, 1.0}
#define AttributeConnectionColor {0.5, 0.5, 0.9, 1.0}
#define MiniButtonAuthoredColor {0.0, 1.0, 0.0, 1.0}
#define MiniButtonUnauthoredColor {0.6, 0.6, 0.6, 1.0}
#define TransparentColor {0.0, 0.0, 0.0, 0.0}
#define PrimInactiveColor {0.7, 0.4, 0.4, 1.0}


/// Decimal Precision shown in the floating point values UI
constexpr const char * DecimalPrecision = "%.5f";

/// Default name when creating a prim
constexpr const char *const DefaultPrimSpecName = "prim";

/// Return error codes
constexpr int ERROR_UNABLE_TO_COMPILE_SHADER = 110;

constexpr const char* ICON_DELETE = "\xef\x87\xb8"; // ICON_FA_TRASH;
