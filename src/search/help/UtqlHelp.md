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

## Instancing

An instanceable prim shares its contents with every other instance of the same
asset, through a hidden prototype. `INSTANCEABLE` is the authored request; the
other fields report what the stage made of it.

| Field | True for |
|---|---|
| `INSTANCEABLE` | prims with `instanceable = true` authored |
| `IS_INSTANCE` | the prim that is an instance |
| `IS_INSTANCE_PROXY` | the prims under an instance, which are its view of the shared contents |
| `IS_PROTOTYPE` | the root of a prototype |
| `IS_IN_PROTOTYPE` | a prototype root and everything under it |

`INSTANCEABLE` works on `USDPRIM` and `SDFPRIM`. The others are composed facts,
so they are compile errors on `SDFPRIM`.

A `USDPRIM` scan goes inside instances and into prototypes, so a mesh shared by
100 instances comes back as 100 proxy rows plus one prototype row. `NOT
IS_INSTANCE_PROXY` removes the copies and lists each unique prim once:

```utql
FIND USDPRIM WHERE TYPE = "Mesh" AND NOT IS_INSTANCE_PROXY
```

The instances themselves:

```utql
FIND USDPRIM WHERE IS_INSTANCE RETURN PATH
```

A prim needs a composition arc to become an instance, so this finds prims marked
instanceable that do not actually instance anything:

```utql
FIND USDPRIM WHERE INSTANCEABLE AND NOT IS_INSTANCE
```

## Metadata

`CUSTOMDATA["key"]` reads one `customData` entry on a prim. The key is case
sensitive, and a colon steps into a nested dictionary. A missing key is `NULL`.

```utql
FIND USDPRIM WHERE CUSTOMDATA["pipeline:reviewState"] = "approved"
```

`CUSTOMDATA.KEYS` is the set of keys on a prim, with nested keys written in the
same colon form, and `HAS_CUSTOMDATA` tests for any entry at all.

```utql
FIND USDPRIM WHERE CUSTOMDATA.KEYS CONTAINS "pipeline:priority"
RETURN PATH, CUSTOMDATA["pipeline:priority"]
```

`assetInfo` has its own fields: `ASSETINFO.NAME`, `.VERSION`, `.IDENTIFIER` and
`.DEPENDENCIES`, plus `HAS_ASSETINFO`. `.IDENTIFIER` is the path as authored, not
resolved, and `.DEPENDENCIES` is a set, matched with `CONTAINS`.

```utql
FIND USDPRIM WHERE HAS_ASSETINFO RETURN PATH, ASSETINFO.NAME, ASSETINFO.VERSION
```

`METADATA["key"]` reads any other registered metadata field by name, on prims,
attributes and relationships.

```utql
FIND USDPRIM WHERE METADATA["documentation"] IS NOT NULL
RETURN PATH, METADATA["documentation"]
```

```utql
FIND USDPRIM WHERE METADATA["hidden"] = true
```

A key that already has its own field is a compile error that names the field:
`METADATA["kind"]` asks for `KIND`. Dictionaries and composition lists are not
available through `METADATA`.

On the `USD*` entities, `CUSTOMDATA` and `METADATA` see authored opinions only, not
fallback values from a schema. On the `SDF*` entities they see what that one spec
authors, so `FIND SDFPRIM` tells you which layer set a value.

`METADATA` and the three scalar `ASSETINFO` fields are written with `SET`, like
`CUSTOMDATA`. `NULL` clears the value.

```utql
UPDATE USDPRIM WHERE PATH = "/World/Hero" SET ASSETINFO.VERSION = "2", METADATA["documentation"] = "Approved hero"
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

`ANONYMOUS` is true for layers that live only in memory, with no file behind
them. `NOT ANONYMOUS` keeps the layers saved on disk:

```utql
FIND LAYER WHERE NOT ANONYMOUS RETURN IDENTIFIER, REAL_PATH
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

## Where an edit lands

A statement on a `USD*` entity selects composed objects, so it has to be told
which layer gets the opinion. By default that is the stage's edit target.
`ON LAYER` picks another layer for this statement. It comes after `WHERE`, and the
layer must be in the stage's layer stack.

```utql
UPDATE USDPRIM WHERE NAME LIKE "proxy" ON LAYER "overrides.usda" SET ACTIVE = false
```

```utql
CREATE USDPRIM "/World/Lights/Fill" TYPE "SphereLight" ON LAYER "lighting.usda"
```

A statement on an `SDF*` entity edits the spec it matched, in the layer that holds
that spec. `IN` already decides the layers, so `ON LAYER` is a compile error here.

```utql
UPDATE SDFPRIM IN LAYER "layout.usda" WHERE PATH = "/World/Crate" SET KIND = "component"
```

`LAYER` writes work the same way and set the fields marked in the *Field reference*:

```utql
UPDATE LAYER IN LAYER "shot.usda" SET FRAMES_PER_SECOND = 25
```

`COMPOSING INTO` edits every spec feeding a composed prim, whichever layer holds
it, and takes the place of `IN`. A `WHERE` can then narrow the specs. Deleting this
way is how to remove a composed prim from all the layers that author it:

```utql
UPDATE SDFPRIM COMPOSING INTO "/World/Hero" SET ACTIVE = false
```

```utql
DELETE SDFPRIM COMPOSING INTO "/World/Junk"
```

## Variant authoring

Variants are authored through the `USD*` entities. The specs land at the edit
target, or on the `ON LAYER` layer, and `SDFPRIM` reads can see them afterwards.

`ADD VARIANT` creates a variant, and creates its set first if needed:

```utql
UPDATE USDPRIM WHERE PATH = "/World/Car"
ADD VARIANT["model"] "sedan" ADD VARIANT["model"] "coupe"
```

`INSIDE VARIANT` sends every change in the statement into a variant, creating the
set and the variant if they do not exist. It comes before the changes, after any
`ON LAYER`. This is how each variant gets its own reference:

```utql
UPDATE USDPRIM WHERE PATH = "/World/Car"
INSIDE VARIANT "{model=sedan}" ADD REFERENCE "cars/sedan.usd"
```

```utql
UPDATE USDPRIM WHERE PATH = "/World/Car"
INSIDE VARIANT "{model=sedan}" CREATE ATTRIBUTE "seats" TYPE "int" VALUE 5
```

Authoring into a variant does not select it. `SET VARIANT` does that, and `NULL`
clears the selection:

```utql
UPDATE USDPRIM WHERE PATH = "/World/Car" SET VARIANT["model"] = "sedan"
```

Nested variants are written outermost first, or by adding a set from inside a
variant:

```utql
UPDATE USDPRIM WHERE PATH = "/World/Car"
INSIDE VARIANT "{model=sedan}" ADD VARIANT["trim"] "sport"
```

```utql
UPDATE USDPRIM WHERE PATH = "/World/Car"
INSIDE VARIANT "{model=sedan}{trim=sport}" ADD REFERENCE "cars/sedan_sport.usd"
```

`REMOVE VARIANT` does not exist yet, and `INSIDE VARIANT` cannot be combined with
`SET NAME` or `SET PARENT`.

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

## Field reference

The **Set** column says whether `SET` can write the field: *yes* on both kinds of
entity, *SDF* on the `SDF*` entities only, and *—* for read-only fields. Other
writes have their own form: `SET VALUE`, `ADD` and `REMOVE`, `CREATE ATTRIBUTE`.

`RETURN *` shows every field of the row. `RETURN` also accepts `STAGE` on the
`USD*` entities and `LAYER` on the `SDF*` entities.

**Prims** — `USDPRIM`, `SDFPRIM`

| Field | Meaning | Set |
|---|---|---|
| `NAME` | prim name | SDF |
| `PATH` | full path | — |
| `PARENT` | parent prim path, `/` for root prims | SDF |
| `TYPE` | typed schema, the `Mesh` in `def Mesh` | yes |
| `KIND` | `component`, `assembly`... | yes |
| `SPECIFIER` | `def`, `over` or `class` | SDF |
| `ACTIVE` | the prim is active | yes |
| `ABSTRACT` | the prim is a `class` | — |
| `INSTANCEABLE` | see *Instancing* | yes |
| `DEPTH` | levels below the root; a root prim is 1 | — |
| `CHILD_COUNT` | number of child prims | — |
| `ATTRIBUTE_COUNT` | number of attributes | — |
| `SPEC_COUNT` | specs composing the prim; always 1 on `SDFPRIM` | — |
| `HAS_REFERENCE` `HAS_PAYLOAD` `HAS_VARIANT` `HAS_API` | presence of that family | — |
| `HAS_TIME_SAMPLES` `HAS_SPLINE` `HAS_CLIPS` | the prim has animation of that kind | — |
| `HAS_RELATIONSHIP` | the prim has a relationship | — |
| `RELATIONSHIPS` | set of relationship names, with `CONTAINS` | — |
| `API` `API.COUNT` | set of applied schemas, and how many | — |
| `CUSTOMDATA["key"]` | one `customData` entry | yes |
| `CUSTOMDATA.KEYS` `HAS_CUSTOMDATA` | `customData` keys, and presence | — |
| `ASSETINFO.NAME` `.VERSION` `.IDENTIFIER` | `assetInfo` values | yes |
| `ASSETINFO.DEPENDENCIES` `HAS_ASSETINFO` | dependency set, and presence | — |
| `METADATA["key"]` | any other registered metadata | yes |

Composed only, on `USDPRIM`: `IS_INSTANCE`, `IS_INSTANCE_PROXY`, `IS_PROTOTYPE`,
`IS_IN_PROTOTYPE`, `IS_LOADED`. Authored only, on `SDFPRIM`: `IS_IN_VARIANT`,
`VARIANT_SELECTIONS`. The arc families are listed under *References, payloads and
variants*.

**Attributes** — `USDATTRIBUTE`, `SDFATTRIBUTE`

| Field | Meaning | Set |
|---|---|---|
| `NAME` | full name, with namespace | SDF |
| `BASENAME` | name after the last colon | — |
| `NAMESPACE` | namespace part of the name, such as `primvars` | — |
| `PATH` | full attribute path | — |
| `PARENT` | owning prim path | — |
| `TYPE` | value type, such as `float` | — |
| `VARIABILITY` | `varying` or `uniform` | SDF |
| `INTERPOLATION` | `vertex`, `faceVarying`... | yes |
| `VALUE.SCALAR` | the value, when it is not an array | — |
| `VALUE.IS_ARRAY` | the value is an array | — |
| `VALUE.ARRAY_SIZE` | element count, `-1` on scalars | — |
| `VALUE.BYTE_SIZE` | size of the value in bytes | — |
| `VALUE.IS_BLOCKED` | the value is blocked | — |
| `VALUE.HAS_TIME_SAMPLES` `VALUE.SAMPLE_COUNT` | time samples, and how many | — |
| `VALUE.HAS_SPLINE` | the value is a spline | — |
| `ASSET.IS_MISSING` | the asset path does not resolve | — |
| `HAS_CONNECTION` `CONNECTION.COUNT` | incoming connections, and how many | — |
| `CONNECTION.SOURCE` | set of source attribute paths | — |
| `METADATA["key"]` | registered metadata | yes |

Authored only, on `SDFATTRIBUTE`: `IS_IN_VARIANT`, `VARIANT_SELECTIONS`.

**Relationships** — `USDRELATIONSHIP`, `SDFRELATIONSHIP`

| Field | Meaning | Set |
|---|---|---|
| `NAME` | full name, with namespace | SDF |
| `BASENAME` | name after the last colon | — |
| `NAMESPACE` | namespace part of the name | — |
| `PATH` | full relationship path | — |
| `PARENT` | owning prim path | — |
| `TARGET` | set of target paths, edited with `ADD TARGET` and `REMOVE TARGET` | — |
| `TARGET_COUNT` | number of targets | — |
| `METADATA["key"]` | registered metadata | yes |

Composed only, on `USDRELATIONSHIP`: `TARGET.IS_MISSING`.

**Layers** — `LAYER`

| Field | Meaning | Set |
|---|---|---|
| `IDENTIFIER` | the layer's identifier | — |
| `DISPLAY_NAME` | short name | — |
| `REAL_PATH` | file on disk, `NULL` when anonymous | — |
| `FILE_FORMAT` | `usda`, `usdc`... | — |
| `DIRTY` | has unsaved changes | — |
| `ANONYMOUS` | lives in memory only | — |
| `MUTED` | is muted | yes |
| `EMPTY` | holds no specs | — |
| `IS_ROOT_LAYER` | root layer of an open stage | — |
| `IS_SESSION_LAYER` | session layer of an open stage | — |
| `ROOT_PRIM_COUNT` | number of root prims | — |
| `DEFAULT_PRIM` | default prim | yes |
| `UP_AXIS` | `Y` or `Z` | yes |
| `METERS_PER_UNIT` | scene scale | yes |
| `START_TIME` `END_TIME` | time range | yes |
| `TIMECODES_PER_SECOND` `FRAMES_PER_SECOND` | time rates | yes |
| `HAS_SUBLAYER` `SUBLAYER.COUNT` | sublayers, and how many | — |
| `SUBLAYER.ASSET` `.IS_MISSING` `.LAYER_OFFSET` | the sublayer family | — |

Fields marked *yes* are written with `UPDATE LAYER`.

## Cheat sheet

The four statements:

```
FIND   <entity> [clauses]
UPDATE <entity> [COMPOSING INTO <targets>] [IN <scope>] [AT <time>]
       [WHERE <cond>] [ON LAYER "id"] [INSIDE VARIANT "{set=sel}"]
       <mutation>+ [RETURN <fields>] [LIMIT n]
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
