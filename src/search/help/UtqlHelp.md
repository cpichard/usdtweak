# UTQL

UTQL is a query language for USD scenes. A query asks a question about the
scene and returns a table of rows; clicking a row selects it in the editor. The
same language can also modify the scene, with one undo entry per statement.

The **use** button on an example loads it into the query editor; **run** loads it
and runs it. Lines starting with `#` are comments and are stripped before the
query runs.

## Getting started

A statement starts with one of four keywords:

| Keyword | What it does |
|---|---|
| `FIND` | reads the scene and returns rows |
| `UPDATE` | changes prims, property values, metadata and composition arcs |
| `CREATE` | adds a prim |
| `DELETE` | removes authored specs |

The examples use `FIND` until *Changing the scene*, which covers the other
three.

A `FIND` names the entity to look at. Everything after that is optional.

```utql
FIND USDPRIM WHERE TYPE = "Mesh"
```

`WHERE` filters the rows, `RETURN` chooses the displayed columns, `ORDERED BY`
sorts them, and `LIMIT` caps how many are returned.

```utql
FIND USDPRIM
WHERE TYPE = "Mesh" AND CHILD_COUNT > 0
RETURN PATH, TYPE, CHILD_COUNT
ORDERED BY CHILD_COUNT DESC
LIMIT 20
```

Keywords are case insensitive and strings go in `"double quotes"`. Clauses must
appear in the order listed in the cheat sheet at the end of this page; a clause
out of order is a compile error.

The editor runs one statement at a time; `Ctrl+Enter` runs it.

Without an `IN` clause, a statement covers everything open in usdtweak: every
open stage for the `USD*` entities, every loaded layer for the `SDF*` and `LAYER`
entities. `IN` narrows it to a named stage, a named layer, or the current stage's
layer stack.

## Composed or authored: pick the entity

USD composes many layers into one resolved scene. Each entity looks at one side
of that composition:

|  | composed — the resolved scene | authored — what each layer says |
|---|---|---|
| prims | `USDPRIM` | `SDFPRIM` |
| attributes | `USDATTRIBUTE` | `SDFATTRIBUTE` |
| relationships | `USDRELATIONSHIP` | `SDFRELATIONSHIP` |
| layers | — | `LAYER` |

The `USD*` entities report what the stage resolves to — the composed value, the
effective type, whether an asset actually loads. They return one row per composed
object.

The `SDF*` entities report authored opinions. They return one row per layer
that authors an opinion, with the layer in the row, and answer questions such as
which file to edit, which layer overrides another, or which layer deletes an arc.

The same question both ways — the composed prims that are meshes, then the layers
that author them:

```utql
FIND USDPRIM WHERE TYPE = "Mesh"
```

```utql
FIND SDFPRIM WHERE TYPE = "Mesh" RETURN PATH, LAYER
```

`LAYER` queries the layers themselves rather than the specs inside them; it is
covered under *Layers and stage settings*.

## Finding prims

Prims can be filtered on type, kind, specifier, activation state and position in
the hierarchy.

```utql
FIND USDPRIM WHERE KIND = "component"
```

```utql
FIND USDPRIM WHERE NOT ACTIVE
```

`IS_A` matches a schema family instead of one concrete type, so there is no need
to list `Mesh`, `Sphere`, `Cube` and the rest. Abstract schema names work too: `Gprim`,
`Imageable`, `Xformable`, `Boundable`. Typeless prims never match, and a name
that is not in the schema registry is a compile error rather than an empty result.

```utql
# all geometry, whatever the concrete type
FIND USDPRIM WHERE TYPE IS_A "Gprim"
```

`UNDER` is a namespace test on `PATH`: `PATH UNDER "/World"` matches `/World`
and `/World/Sets` but not `/WorldOther`, which a substring match would also catch.

```utql
FIND USDPRIM WHERE PATH UNDER "/World/Sets"
```

`LIKE` matches a substring, or a regular expression when the pattern is written
between slashes.

```utql
FIND USDPRIM WHERE NAME LIKE "proxy"
```

```utql
FIND USDPRIM WHERE PATH LIKE /\/World\/Sets\/sh\d{4}_/
```

Applied API schemas are not part of the prim type. `API` is a set of schema
names, matched with `CONTAINS`.

```utql
FIND USDPRIM WHERE API CONTAINS "PhysicsCollisionAPI"
```

An absent value is `NULL`, not an empty string.

```utql
FIND USDPRIM WHERE TYPE IN ("Mesh", "Camera") AND KIND IS NULL
```

## Attributes and values

Attribute entities carry the value: `VALUE.SCALAR` for a single value, and
`VALUE.IS_ARRAY`, `VALUE.ARRAY_SIZE` and `VALUE.BYTE_SIZE` for arrays.

```utql
FIND USDATTRIBUTE
WHERE NAME = "points" AND VALUE.IS_ARRAY AND VALUE.ARRAY_SIZE > 100000
ORDERED BY VALUE.ARRAY_SIZE DESC
LIMIT 50
```

`VALUE.SCALAR` follows the value type: numbers compare with `<`, `>`, `=` and
the rest, strings and tokens with `=` and `LIKE`, and a bool works as a condition
on its own. It does not apply to array values.

`NAME` is the full property name including its namespace; `BASENAME` is the part
after the last colon. Shading and light parameters are namespaced under `inputs:`
(`inputs:intensity`) while plain geometry attributes are not (`points`,
`visibility`, `purpose`), so `BASENAME` finds either kind without knowing which.

```utql
FIND USDATTRIBUTE WHERE BASENAME = "intensity" AND VALUE.SCALAR > 1000
```

`AT` reads values at a given frame, and only attribute entities accept it.
Without it, stage reads use the editor's current time.

```utql
FIND USDATTRIBUTE AT TIME 24 WHERE BASENAME = "visibility" AND VALUE.SCALAR = "invisible"
```

A value can be animated by time samples, by a spline, or by value clips. The
three are independent, so finding everything animated means testing all three:

```utql
FIND USDPRIM WHERE HAS_TIME_SAMPLES OR HAS_SPLINE OR HAS_CLIPS
```

Attributes with a blocked value:

```utql
FIND USDATTRIBUTE WHERE VALUE.IS_BLOCKED
```

## Broken assets and dangling targets

`ASSET.IS_MISSING` is true when an `asset`-valued attribute does not resolve.
It calls the asset resolver, which touches the file system, so on a large stage
it pays to narrow the query with `IN`.

```utql
FIND USDATTRIBUTE WHERE ASSET.IS_MISSING RETURN PATH, VALUE.SCALAR
```

References that do not resolve:

```utql
FIND USDPRIM WHERE REFERENCE.IS_MISSING RETURN PATH, REFERENCE.ASSET
```

When `WHERE` filters on an arc family, a `RETURN` field from the same family
shows only the arcs that matched: a prim with one broken and two working
references lists only the broken one.

Relationships pointing at prims that do not exist on the stage:

```utql
FIND USDRELATIONSHIP WHERE TARGET.IS_MISSING RETURN PATH, TARGET
```

Payloads that are not loaded. Deactivated prims also report as not loaded, so
`ACTIVE` leaves them out:

```utql
FIND USDPRIM WHERE HAS_PAYLOAD AND NOT IS_LOADED AND ACTIVE
```

`REFERENCE.IS_MISSING` means "the prim has at least one missing reference", so
its negation also matches every prim with no reference at all. Add
`HAS_REFERENCE` for "has references, and none are broken":

```utql
FIND USDPRIM WHERE HAS_REFERENCE AND NOT REFERENCE.IS_MISSING
```

## References, payloads and variants

Composition arcs are grouped into families, each with its own fields:

| Family | Fields |
|---|---|
| `REFERENCE`, `PAYLOAD` | `.ASSET` `.PRIM_PATH` `.IS_MISSING` `.LAYER_OFFSET` `.LAYER_SCALE` `.OP` |
| `INHERIT`, `SPECIALIZE` | `.PRIM_PATH` `.OP` |
| `VARIANT` | `.SET` `.SELECTION` |
| `API` | `API CONTAINS "Name"`, `API.COUNT`, `API.OP` |

`HAS_REFERENCE`, `HAS_PAYLOAD`, `HAS_VARIANT` and `HAS_API` test for presence.
On a family field, `CONTAINS` tests for an exact value on any of the prim's arcs;
use `LIKE` for a substring.

```utql
FIND USDPRIM WHERE REFERENCE.ASSET LIKE "bob.usd" RETURN PATH, REFERENCE.ASSET
```

A prim can hold several arcs of the same family. Conditions on one family joined
by `AND` must all hold on the same arc, not on different arcs of the prim:

```utql
# .OP is authored-only, so this is an SDF query
FIND SDFPRIM
WHERE REFERENCE.ASSET LIKE "bob.usd" AND REFERENCE.OP = "delete"
RETURN PATH, LAYER
```

`VARIANT.SET` and `VARIANT.SELECTION` describe the variant sets defined on a prim
and the selection each one currently has:

```utql
FIND USDPRIM WHERE VARIANT.SET CONTAINS "shadingVariant" RETURN PATH, VARIANT.SELECTION
```

`IS_IN_VARIANT` answers a different question: which specs are authored *inside*
a variant. That is an authored fact, so it exists only on `SDFPRIM` and
`SDFATTRIBUTE`, together with `VARIANT_SELECTIONS`, the `{set=value}` scopes the
spec sits under.

```utql
FIND SDFPRIM WHERE IS_IN_VARIANT RETURN PATH, VARIANT_SELECTIONS, LAYER
```

## Layers and stage settings

`FIND LAYER` returns one row per layer. Stage settings — up axis, frame rate,
default prim — are stored on the root layer.

```utql
FIND LAYER WHERE IS_ROOT_LAYER RETURN IDENTIFIER, UP_AXIS, FRAMES_PER_SECOND, DEFAULT_PRIM
```

Layers with unsaved changes:

```utql
FIND LAYER WHERE DIRTY RETURN IDENTIFIER, REAL_PATH
```

Sublayers form a family like the prim arcs, with `SUBLAYER.ASSET`,
`SUBLAYER.IS_MISSING` and `SUBLAYER.LAYER_OFFSET`. The layers that sublayer a
given file, then the sublayers that do not resolve:

```utql
FIND LAYER WHERE SUBLAYER.ASSET LIKE "layout.usda" RETURN IDENTIFIER, SUBLAYER.ASSET
```

```utql
FIND LAYER WHERE SUBLAYER.IS_MISSING RETURN IDENTIFIER, SUBLAYER.ASSET
```

## Following connections

`CONNECTED TO` walks the attribute connection graph and returns the prims it
reaches, excluding the origin. `CONNECTED TO` ignores direction and returns the
whole connected network; `CONNECTED UPSTREAM OF` returns what feeds the origin,
`CONNECTED DOWNSTREAM OF` what it feeds. `WITHIN n` stops the walk after *n* prim
hops. The origin can be a prim path, an attribute path, or a named result.

```utql
FIND USDPRIM CONNECTED TO "/Looks/Mat/Surface"
```

```utql
FIND USDPRIM CONNECTED UPSTREAM OF "/Looks/Mat.outputs:surface"
```

`CONNECTED` comes before `WHERE`, which then filters the prims the walk reaches:

```utql
FIND USDPRIM CONNECTED TO "/Looks/Mat" WITHIN 2 WHERE TYPE = "Shader"
```

To see the incoming connections of individual attributes, query the attributes
instead:

```utql
FIND USDATTRIBUTE WHERE HAS_CONNECTION RETURN PATH, CONNECTION.SOURCE
```

## Which layer authors this?

`COMPOSING INTO` takes composed objects and returns the authored specs feeding
them — the way to get from a composed result to the files that produce it.

Since the editor runs one statement at a time, this takes two runs: `AS` stores
the first result under a name, and the second statement reads it back with
`RESULTSET`. A named result stays available until usdtweak closes or another `AS`
reuses the name.

```utql
FIND USDPRIM WHERE NAME LIKE "hero" AS "heroes"
```

```utql
FIND SDFPRIM COMPOSING INTO RESULTSET "heroes" RETURN PATH, LAYER
```

By default the result has one row per unique spec. `PER TARGET` returns one row
per target and spec pair, and adds three fields: `COMPOSITION.TARGET`,
`COMPOSITION.ARC_TYPE` and `COMPOSITION.STRENGTH`. A strength of `0` is the
strongest opinion, the one that wins.

```utql
FIND SDFPRIM COMPOSING INTO RESULTSET "heroes" PER TARGET
WHERE COMPOSITION.STRENGTH = 0
RETURN COMPOSITION.TARGET, PATH, LAYER, COMPOSITION.ARC_TYPE
```

`COMPOSED FROM` goes the other way, from an authored spec to the composed prims
it feeds. A spec referenced from several places lands at several
composed paths, which a `PATH =` filter would miss.

```utql
FIND USDPRIM COMPOSED FROM LAYER "modeling.usd" PATH "/root/chair"
```

## Changing the scene

`UPDATE`, `CREATE` and `DELETE` use the same entities, scopes and `WHERE`
conditions as `FIND`. A good way to write an edit is to get a `FIND` returning
the right rows, then change the verb and add the change. Each statement is one
undo entry, and the **Dry run** checkbox reports what a statement would do
without authoring anything.

`UPDATE` and `DELETE` require a `WHERE`, an `IN RESULTSET`, or a named `IN LAYER`
scope; an edit meant to cover the whole scene has to be written out as
`WHERE PATH UNDER "/"`.

Scoping matters more on a write than on a read: like `FIND`, an `UPDATE` or
`DELETE` with no `IN` clause covers every open stage or every loaded layer, not
just the current one. Add `IN STAGE "id"` or `IN LAYER "id"` to confine
the edit, and use **Dry run** to check the row count first.

```utql
UPDATE USDPRIM WHERE NAME LIKE "proxy" SET ACTIVE = false
```

```utql
UPDATE USDATTRIBUTE WHERE BASENAME = "intensity" AND VALUE.SCALAR > 5000 SET VALUE = 5000
```

`SET VALUE = NULL` clears the authored opinion, `SET VALUE = BLOCK` authors a
value block, and `AT TIME` writes a time sample instead of the default value.

```utql
UPDATE USDATTRIBUTE AT TIME 12 WHERE NAME = "xformOp:translate" SET VALUE = (0, 10, 0)
```

`SAMPLES` authors several keyframes in one statement and one undo entry:

```utql
UPDATE USDATTRIBUTE WHERE PATH = "/World/Ball.xformOp:translate"
SET VALUE = SAMPLES {1: (0, 0, 0), 12: (0, 5, 0), 24: (0, 0, 0)}
```

`CREATE` adds a prim. `CREATE USDPRIM` always targets the current stage, the
only statement that does; `CREATE SDFPRIM` requires an `IN LAYER` destination.
Attributes and relationships are not made with `CREATE` on its own: an `UPDATE`
adds them to each matched prim with a `CREATE ATTRIBUTE` or `CREATE RELATIONSHIP`
clause.

```utql
CREATE USDPRIM "/World/Lights/Key" TYPE "SphereLight"
```

```utql
UPDATE USDPRIM WHERE PATH = "/World/Lights/Key" CREATE ATTRIBUTE "inputs:intensity" TYPE "float" VALUE 500.0
```

`DELETE` removes authored specs, so it runs on `SDF*` entities. To make a
composed prim disappear, deactivate it or delete the specs that compose it.

```utql
DELETE SDFPRIM IN LAYER "layout.usda" WHERE PATH UNDER "/World/Junk"
```

`ADD` and `REMOVE` edit composition arcs. When the `WHERE` filters on the same
family, `REMOVE` removes only the arcs that matched and leaves the prim's other
arcs alone.

```utql
UPDATE USDPRIM WHERE PATH = "/World/Set" ADD REFERENCE "sets/kitchen.usd"
```

```utql
UPDATE USDPRIM WHERE REFERENCE.IS_MISSING REMOVE REFERENCE
```

```utql
UPDATE USDRELATIONSHIP WHERE TARGET.IS_MISSING REMOVE TARGET
```

Metadata can be written one key at a time or as a whole dictionary. Setting the
whole dictionary replaces it, and `NULL` clears it.

```utql
UPDATE USDPRIM WHERE KIND = "component" SET CUSTOMDATA["pipeline:reviewState"] = "approved"
```

```utql
UPDATE USDPRIM WHERE PATH = "/World/Hero" SET CUSTOMDATA = {"assetType": "prop", "pipeline:priority": 5}
```

Renaming and reparenting change authored namespace, so they run on `SDF*`
entities. Paths are updated inside the edited layer only; opinions in other
layers keep pointing at the old path.

```utql
UPDATE SDFPRIM IN LAYER "layout.usda" WHERE NAME = "Cube" SET NAME = "Crate"
```

## Common mistakes

- **Applied APIs are a set.** Use `API CONTAINS "MaterialBindingAPI"`, not
  `TYPE = "MaterialBindingAPI"`. `TYPE` is the prim's typed schema, the `Mesh` in
  `def Mesh`.
- **`NOT` on an arc family also matches prims that have no such arc.**
  `NOT REFERENCE.IS_MISSING` includes prims with no references; pair it with
  `HAS_REFERENCE`.
- **`VALUE.ARRAY_SIZE` is `-1` on scalars,** so `VALUE.ARRAY_SIZE < 10` also
  matches every scalar. Add `VALUE.IS_ARRAY` when arrays are meant.
- **An absent value is `NULL`**, not `""` — write `KIND IS NULL`.
- **`AT` applies to attributes only.** On prims, relationships or layers it is a
  compile error.
- **`.OP` is authored-only.** It exists on `SDF*` entities; on `USD*` it is a
  compile error, because the composed stage only sees the resulting arcs.
- **`RETURN` changes the displayed columns, never which rows match.**
- **One statement per run.** Two statements in the editor are a compile error;
  run them one after the other.

## Cheat sheet

The four statements:

```
FIND   <entity> [clauses]
UPDATE <entity> [targeting] <mutation>+
CREATE USDPRIM "/path" [TYPE "t"] [ON LAYER "id"]
CREATE SDFPRIM "/path" IN LAYER "id" [SPECIFIER "def"|"over"|"class"] [TYPE "t"]
DELETE SDFPRIM|SDFATTRIBUTE|SDFRELATIONSHIP (COMPOSING INTO <target> | [IN <scope>] WHERE <cond>)
```

`FIND` clauses: only `FIND <entity>` is required, and the clauses used must
come in this order:

```
FIND <entity>
     [COMPOSING INTO <targets> [PER TARGET]]
     [COMPOSED FROM <origin>]
     [CONNECTED [UPSTREAM|DOWNSTREAM] (TO|OF) <origin> [WITHIN n]]
     [IN <scope>]
     [AT <time>]
     [WHERE <condition>]
     [RETURN <fields> | *]
     [ORDERED BY <field> [ASC|DESC], ...]
     [LIMIT <int>]
     [AS "<name>"]
```

Conditions:

| Form | Meaning |
|---|---|
| `field = literal` | also `!=` `<` `<=` `>` `>=` |
| `field LIKE "text"` | substring match |
| `field LIKE /regex/` | regular expression |
| `field IN ("a", "b")` | one of these values |
| `set CONTAINS "value"` | membership in a set-valued field (`API`, `TARGET`, arc families) |
| `field IS NULL` | also `IS NOT NULL` |
| `TYPE IS_A "Gprim"` | schema-family test, prims only |
| `PATH UNDER "/World"` | namespace prefix |
| `PATH UNDER RESULTSET "n"` | at or under any member of a named result |
| `ACTIVE` | a bool field on its own is a condition |

Combine with `AND`, `OR`, `NOT` and parentheses; `NOT` binds tightest, then
`AND`, then `OR`.

Scopes — `IN` narrows a statement; with no `IN` it covers everything open:

| Scope | Meaning |
|---|---|
| *(omitted)* | every open stage (`USD*`) / every loaded layer (`SDF*`, `LAYER`) |
| `IN STAGE "id"` | one named stage (its root layer identifier) |
| `IN STAGES "a;b"` | several stages; `IN STAGES "*"` for all open ones |
| `IN LAYER "id"` | one named layer (`SDF*` and `LAYER` entities) |
| `IN LAYERS "a;b"` | several named layers |
| `IN LAYERSTACK` | the current stage's whole layer stack |
| `IN SUBLAYERS` | direct sublayers of the current root layer |
| `IN RESULTSET "n"` | the paths of a previously named result |
