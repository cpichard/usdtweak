# Twiki scene assistant

## Presentation
Twiki is an LLM agent that has a view and a limited understanding of your opened stages and layers. It can help you find issues and make bulk modifications to the scene, using natural language. As of today it can only connect to Claude via an API key. Some examples of use here : https://www.youtube.com/playlist?list=PLF87-EA-D-Pw

## Setup
Even though it has been architected to work with any type of model — with the ultimate goal of supporting local models — today it only works with Claude, and you need an Anthropic API key to use it. To get this key, create a developer account on the Anthropic website, go to the API section, and follow the prompts to generate a new API key. The developer account is different from the regular account. In the future we should be able to connect to other LLM providers. Before launching usdtweak, set the environment variable with your key, all you'll be all set:

```
   export ANTHROPIC_API_KEY="sk-ant-api03-9tosdkpwoejflskdnnowiejfoisdldnfiwheoihosdf...."
```

You can also set the model with the environment variable `ANTHROPIC_MODEL`. By default it is set to `claude-sonnet-4-6`.

## Design

Twiki relies on an internal query language, "utql", to search the scene; Open the Twiki panel, type a request in plain English, and press enter. For example:

> Select all the meshes in the scene and make them invisible.

Behind the scenes Twiki translates your request into a utql query, runs it inside usdtweak, and keeps the matching prims in a named list. A query like the one above looks like:

```
FIND USDPRIM WHERE TYPE = "Mesh"
```

The result stays in usdtweak, so the follow-up edit (hiding the prims) is applied as a single, undoable operation — no need to send every path back and forth to the LLM. You can keep refining in the same conversation ("now only the ones under /World/props"), and the token count for each exchange is shown in the panel.

UTQL is close to natural language and SQL, this query language was designed for several reasons:

 - LLMs are trained on natural language and on structured languages like SQL, so having a query language close to those allows for an easy translation of questions into queries. Many frontier LLMs were trained to write USD Python code, so Python is basically "free", but the output for the resulting Python code is always an order of magnitude larger than with utql: it is costlier to retrieve (output tokens are more expensive), slower to run, and can't be undone/redone.

- When the llm inspect the scene, it looks for a particular path or set of paths, sending each and every path and its children to the LLM can take many rounds of tool execution plus LLM processing and be excrutiatingly slow. It also bloats the context. This doesn't work well with questions like "find all the meshes in the scene" on production sized scenes.

- The internal query language keeps the results inside usdtweak and saves them in named lists that can be reused by any tool for later processing. This allows bulk modifications, inspection, and undo/redo.

- How many tools an LLM can handle before it gets confused and starts making mistakes is apparently around 50 today; we want to keep the number of tools to a minimum, and a search tool driven by a query language allows that. A query language is also more flexible than having multiple specialized functions, and avoids an explosion in the number of arguments.

## Security
The information in your scene is only sent to your Anthropic account; usdtweak doesn't store anything.

## Caveats

As a first version, Twiki can search stages and layers but can only edit stages in edit targets. There are probably elements that can't be edited at the moment, please file a github issue if you find any, that will help improve Twiki.

LLMs are probabilistic machines, and there is always a chance that your prompt won't work. Like slot machines, you'll win and you'll lose, you'll get the thrill and play again. The Monte-Carlo casino will always get your money in the end — and with that in mind, the number of tokens used is shown in the Twiki panel. It can be costly, be aware of that. To improve Twiki and make it more efficient token wise, we would love to know the failing use cases, don't hesitate to file and issue on github.

Twiki is not an MCP server, it is an internal agent. We didn't need MCP so far, but if your workflow needs Twiki as an MCP server, please file an issue.