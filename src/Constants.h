#pragma once
///
/// Constants used in the application
///

/// PI
constexpr float PI_F = 3.14159265;

/// Size of the main window when it opens.
constexpr int InitialWindowWidth = 1024;
constexpr int InitialWindowHeight = 1024;

/// Height of a row in the property editor
constexpr float TableRowHeight = 22.f;

/// Waiting time before a tooltip shows up
constexpr float TimeBeforeTooltip = 2.f; // 2 seconds

/// Predefined colors for the different widgets
#define ColorAttributeAuthored {1.0, 1.0, 1.0, 1.0}
#define ColorAttributeUnauthored {0.5, 0.5, 0.5, 1.0}
#define ColorAttributeRelationship {0.5, 0.5, 0.9, 1.0}
#define ColorAttributeConnection {0.5, 0.5, 0.9, 1.0}
#define ColorMiniButtonAuthored {0.0, 1.0, 0.0, 1.0}
#define ColorMiniButtonUnauthored {0.6, 0.6, 0.6, 1.0}
#define ColorTransparent {0.0, 0.0, 0.0, 0.0}
#define ColorPrimDefault {227.f/255.f, 227.f/255.f, 227.f/255.f, 1.0}
#define ColorPrimInactive {0.4, 0.4, 0.4, 1.0}
#define ColorPrimInstance {135.f/255.f, 206.f/255.f, 250.f/255.f, 1.0}
#define ColorPrimPrototype {118.f/255.f, 136.f/255.f, 217.f/255.f, 1.0}
#define ColorPrimHasComposition {222.f/255.f, 158.f/255.f, 46.f/255.f, 1.0}
#define ColorGreyish {0.5, 0.5, 0.5, 1.0}
#define ColorButtonHighlight {0.5, 0.7, 0.5, 0.7}

/// Decimal Precision shown in the floating point values UI
constexpr const char * DecimalPrecision = "%.5f";

/// Default name when creating a prim
constexpr const char *const DefaultPrimSpecName = "prim";

/// Return error codes
constexpr int ERROR_UNABLE_TO_COMPILE_SHADER = 110;

constexpr const char* ICON_DELETE = "\xef\x87\xb8"; // ICON_FA_TRASH;

