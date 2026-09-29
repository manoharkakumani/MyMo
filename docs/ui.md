# MyMo UI

`ui` is MyMo's app library. You describe screens with indented blocks,
and the same code runs as a browser app, a desktop window, an app on
your phone, or a full-screen terminal program.

```python
from "ui" use *

count = 0

page "/":
    center:
        text count, size="display"
        row:
            button "−": count -= 1
            button "+" primary: count += 1

run "Counter"
```

```bash
mymo counter.my              # opens in your browser
mymo counter.my --desktop    # its own window
mymo counter.my --phone      # on your phone, over Wi-Fi
mymo counter.my --term       # in the terminal
```

## Contents

1. [Quick start](#1-quick-start)
2. [How it works](#2-how-it-works)
3. [The syntax](#3-the-syntax)
4. [Data and state](#4-data-and-state)
5. [Handlers](#5-handlers)
6. [Pages and navigation](#6-pages-and-navigation)
7. [Components](#7-components)
8. [Styling](#8-styling)
9. [Calling APIs and backends](#9-calling-apis-and-backends)
10. [Background work, timers and live updates](#10-background-work-timers-and-live-updates)
11. [Where your app runs](#11-where-your-app-runs)
12. [Projects](#12-projects)
13. [Testing](#13-testing)
14. [Performance](#14-performance)
15. [Gotchas](#15-gotchas)
16. [Plain-call style](#16-plain-call-style)

---

## 1. Quick start

Create a project and start it:

```bash
mymo new my-app
cd my-app
mymo app.my --dev
```

`--dev` opens the app in your browser and reloads it every time you save
a file. The project has a home page, a page that loads data from a public
API, and an about page. Change any of them and watch the browser update.

A single file works too:

```python
# hello.my
from "ui" use *

name = state("")

page "/":
    card "Hello":
        input name, placeholder="Your name"
        text "Hi, {name}!" if name.value != "" else "Type your name."

run "Hello"
```

```bash
mymo hello.my
```

---

## 2. How it works

Your code runs in the MyMo process on your machine, not in the browser.
The browser only shows what that code produces.

1. A **page** is a block that describes the screen. MyMo runs it and
   draws the result.
2. When the user clicks, types or navigates, the matching **handler**
   runs. It changes your data (a variable, a list, a `state`).
3. The page block runs again, and the browser patches only the parts that
   changed. Unchanged elements keep their state: focus, what's being
   typed, scroll position, running animations.

You never update the screen by hand. You change data, and the screen
follows.

Some consequences worth knowing:

- **Anything MyMo can do, a handler can do**: call APIs, read files,
  query a database, run commands. No API layer, no CORS, and your API keys
  never reach the browser.
- **A page block should only describe the screen.** It runs often (after
  every interaction), so do the work in handlers, `spawn` or `every`, not
  in the page.
- **Data is shared.** Every window and device that opens the app sees the
  same data, the way a desktop app has one set of data. A change in one
  window shows up in the others immediately.

---

## 3. The syntax

### Components are commands

A line that starts with a name followed by arguments is a call, and
the parentheses are optional:

```python
h1 "Settings"                            # h1("Settings")
text "3 new messages"                    # text("3 new messages")
avatar "Ada Lovelace", size=48           # avatar("Ada Lovelace", size=48)
```

- **Arguments** are separated by commas.
- **Keyword arguments** are `key=value`. Right after an argument the comma
  is optional: `button "Export" icon="download"`.
- **Flags** are bare words right after an argument: `button "Save" primary
  small` means `button("Save", primary=True, small=True)`.
- **Strings with `{...}` interpolate** in command arguments:
  `text "Hello, {name}!"`. For a literal brace, write `\{`. Inside the
  braces, use single quotes for strings: `"{user['name']}"`.

### Blocks

A line ending in `:` passes the indented block below it (or the rest of
the line) to the component:

```python
card "Profile":                  # the block holds the card's children
    avatar "Ada Lovelace"
    text "Mathematician" muted

button "Save" primary: save()    # the block is the click handler
```

On containers the block holds the **children**. Everything created
inside it becomes a child, including components made in `for` loops, `if`
branches or helper functions. On buttons and inputs the block is the
**handler**. Indent blocks by 4 spaces.

### When you still need parentheses

Command calls only work at the start of a line. Anywhere else, call
functions as usual:

```python
n = len(todos)                        # a value
text "Total: {sum(prices)}"           # inside an interpolation
row(badge("new"), badge("sale"))      # a call nested in a call
todos.clear()                         # a method call
spacer()                              # no arguments, inside a block
```

---

## 4. Data and state

**Plain variables** hold your data. Pages read them; handlers change
them.

```python
todos = []
filter = "all"
```

**`state(value)`** is for values an input edits. An input needs
somewhere to write what the user types, and a plain variable is passed
by value, so wrap it:

```python
name = state("")
agree = state(False)
size = state("M")

input name
checkbox "I agree", agree
select ["S", "M", "L"], size
```

Read a state with `name.value`, or inside a string with `"{name}"`.
Change it from handlers with:

| Method | Does |
| --- | --- |
| `s.set(v)` | `s.value = v` |
| `s.inc(n=1)` / `s.dec(n=1)` | add / subtract |
| `s.toggle()` | flip a bool |
| `s.push(x)` / `s.remove(x)` | edit a list |
| `s.clear()` | empty string or list |

Create states at the top level of a file, not inside a page block,
because the page block runs again on every change.

---

## 5. Handlers

A handler is the block after a button, checkbox, input and so on:

```python
button "+": count += 1

button "Save" primary:
    save_profile()
    toast "Saved" success
```

**Assignments in a handler block change the outer variable.**
`count += 1` updates the `count` defined at the top of the file, with no
`global` needed.

**In a loop, each handler keeps its own item:**

```python
for t in todos:
    row:
        text t["title"]
        button "Delete": todos.remove(t)    # deletes this row's t
```

**Handlers can be functions.** Call them from the block (`: save()`), or
pass them with `on=`:

```python
button "Save", on=save
button "Delete", on=(delete_item, item)     # (function, arguments...)
```

**Inside a normal `fn`, use `global`** to change a top-level variable,
as in Python:

```python
fn reset():
    global count
    count = 0
```

**What handlers can use:**

| Call | Does |
| --- | --- |
| `toast "Saved" success` | a notification (`success`, `error`, `warning`, default info) |
| `go "/settings"` | switch pages |
| `sleep 1.5` | wait without freezing the app |

Handler errors don't crash the app. They show up as an error toast.

---

## 6. Pages and navigation

Each `page "/path":` block is a screen:

```python
page "/":
    h1 "Home"
    link "Settings", "/settings"

page "/settings":
    h1 "Settings"
```

- `link "Label", "/path"` and `nav "Label", "/path", icon="home"` switch
  pages without reloading. `link "Docs", "https://…", external=True` opens a
  new tab.
- `go "/path"` switches from a handler.
- The browser's back and forward buttons work.
- The first page registered is the home page if there's no `"/"`.
  Unknown paths show a "page not found" screen.
- `page "/about", title="About":` sets the tab title to `About · App name`.

**App chrome.** `shell:` with a `sidebar` or `navbar` as its first child
gives every page the same frame:

```python
fn layout(title, body=Nil):
    shell:
        sidebar "Northwind":
            nav "Overview", "/", icon="home"
            nav "Customers", "/customers", icon="users"
        h1 title
        if body:
            body()

page "/":
    layout "Overview":
        text "Welcome back."
```

`layout` is a component you wrote yourself: a function that takes a block
(`body`). On a phone the sidebar becomes a top bar with icons.

---

## 7. Components

Every component also accepts the [layout props](#layout-props) and
`style=`/`cls=` (see [Styling](#8-styling)).

### Layout

| Component | Notes |
| --- | --- |
| `col:` | children stacked vertically (the default inside cards and pages) |
| `row:` | children side by side, wrapping on small screens |
| `grid cols=3:` | a grid; without `cols`, as many columns of at least `min=220` px as fit |
| `center:` | centred horizontally and vertically |
| `card "Title", subtitle="…":` | a surface with padding, border and shadow |
| `section "Title", subtitle="…":` | a titled group without a surface |
| `spacer()` | pushes the following items in a row to the right |
| `divider()` / `divider "or"` | a horizontal rule, optionally labelled |
| `shell:` | app frame; first child `sidebar` or `navbar` |

### Text

| Component | Notes |
| --- | --- |
| `h1 "…"`, `h2`, `h3` | headings |
| `text "…"` | flags `muted`, `bold`, `center`; `size="xs|sm|lg|xl|display"`; `color="green"` |
| `small "…"` | small muted text |
| `code "x = 1"` / `code src, block=True` | inline code / a code block |
| `link "Label", "/to"` | `external=True`, `icon=` |
| `bullets ["a", "b"]` | a bulleted list (or a block of children) |
| `kv {"Plan": "Pro", "Seats": 12}` | label/value rows |

### Display

| Component | Notes |
| --- | --- |
| `badge "New", color="green"` | a small pill |
| `avatar "Ada Lovelace"` / `avatar src="/static/me.jpg"` | initials on a colour from the name; `size=40` |
| `image "/static/photo.jpg", alt="…"` | `height=`, `round` |
| `icon "star", size=18, color="amber"` | see [icons](#icons) |
| `stat "Revenue", "$48k", delta="+12%", icon="dollar", hint="vs last week"` | a KPI card; a `delta` starting with `-` is red |
| `progress 72, label="Storage", color="amber"` | `max=100` |
| `table rows` | rows are dicts (keys become columns) or lists; `columns=[…]` picks and orders; cells can be components |
| `alert "Saved"` | flags `success`, `warning`, `error`; `title=` |
| `empty "No results", "Try another search.", icon="search"` | an empty state; `action=` or a block adds a button |
| `spinner "Loading…"` | activity indicator |

### Input

| Component | Notes |
| --- | --- |
| `button "Label": …` | flags `primary`, `danger`, `ghost`, `small`, `large`, `full`, `disabled`; `icon=`; `button icon="x": …` is icon-only |
| `input state: …` | the block runs on Enter; `label=`, `placeholder=`, `icon=`, `type="email"`, flags `password`, `live` (update the page as you type) |
| `textarea state` | `rows=4`, `label=`, `live` |
| `checkbox "Label", state_or_bool: …` | with a state it's bound; with a plain bool the block does the toggling |
| `switch "Label", state` | same as checkbox, drawn as a toggle |
| `select ["a", "b"], state` | options can be `("value", "Label")` pairs; `label=`; block runs on change |
| `slider state, min=0, max=100, step=1, label="…"` | block runs on change |
| `tabs ["Day", "Week"], state` | segmented control; block runs on change |

### Chrome and overlays

| Component | Notes |
| --- | --- |
| `sidebar "Brand":` / `navbar "Brand":` | holds `nav` items |
| `nav "Label", "/to", icon="home"` | highlighted on its own page |
| `modal open_state, "Title":` | shown while the state is truthy; Esc, the × button or a click outside closes it (sets it to False) |
| `toast "…" success` | from handlers |

### Escape hatches

| Component | Notes |
| --- | --- |
| `html "<b>raw</b>"` | inserted as-is (never with user input) |
| `el "section", id="hero", class_="x":` | any HTML element with attributes and children |

### Icons

`plus`, `minus`, `x`, `check`, `chevron-right`, `chevron-left`,
`chevron-down`, `chevron-up`, `arrow-right`, `arrow-left`, `search`,
`home`, `user`, `users`, `settings`, `trash`, `edit`, `star`, `heart`,
`bell`, `mail`, `calendar`, `clock`, `chart`, `trend-up`, `trend-down`,
`menu`, `info`, `alert`, `check-circle`, `sun`, `moon`, `folder`, `file`,
`download`, `upload`, `logout`, `lock`, `zap`, `globe`, `list`, `grid`,
`refresh`, `inbox`, `dollar`, `cart`, `send`, `play`, `pause`, `eye`,
`sparkles`, `filter`, `copy`, `external`, `message`, `book`, `layers`,
`tag`, `map-pin`, `shield`, `terminal`, `code`, `image`, `card`, `box`,
`circle`. The terminal shows a matching symbol.

### Your own components

A component is a function. Take `body=Nil` to accept a block:

```python
fn price_card(plan, price, body=Nil):
    card plan:
        text price, size="xl" bold
        if body:
            body()
        button "Choose {plan}" primary full: choose(plan)

page "/pricing":
    grid cols=3:
        price_card "Starter", "$0":
            bullets ["1 project", "Community support"]
        price_card "Pro", "$12":
            bullets ["Unlimited projects", "Email support"]
```

---

## 8. Styling

### The design system

Components come styled with consistent spacing, type, radii, shadows,
hover and focus states, light and dark themes, and layouts that adapt to
phones. Most apps only pick an accent colour and a theme:

```python
run "Shop", accent="#10b981", theme="auto"     # "auto" | "light" | "dark"
```

The accent colours primary buttons, links, switches, progress bars, the
active nav item and the app icon.

### Flags and props

```python
button "Delete" danger small
text "Updated 2m ago" muted
text "$48,290", size="display"
badge "Trial", color="amber"
```

**Colours:** `accent`, `blue`, `green`, `red`, `amber`, `violet`,
`pink`, `teal`, `gray`, `orange`, or any CSS colour (`"#ff6b6b"`,
`"rgb(…)"`).

### Layout props

All components take:

| Prop | Meaning |
| --- | --- |
| `gap=` | space between children |
| `pad=` | inner padding |
| `width=`, `height=`, `max=` | sizes (`max` is max-width) |
| `grow=True` | take the remaining space in a row |
| `align=` | cross-axis: `start`, `center`, `end`, `stretch`, `baseline` |
| `justify=` | main axis: `start`, `center`, `end`, `between`, `around`, `evenly` |

`gap` and `pad` take a step on the spacing scale or any CSS length. A
number is pixels (`max=560`), and a string is used as-is
(`width="50%"`).

| Step | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| px | 0 | 4 | 8 | 12 | 16 | 20 | 24 | 32 | 40 | 48 | 64 |

### Your own CSS

- **`static/app.css`** in a project is loaded automatically, after the
  built-in styles, so your rules win.
- **`css ".hero { padding: 64px }"`** adds rules from code.
- **`cls="hero"`** adds class names to a component. **`style="…"`** sets
  inline CSS.
- **Theme variables** can be overridden in your CSS: `--accent`, `--bg`,
  `--surface`, `--surface-2`, `--border`, `--text`, `--muted`, `--r`
  (radius), `--r-lg`, `--font`, `--mono`, and the named colours
  (`--green`, …).

```css
/* static/app.css */
:root { --font: "Georgia", serif; --r-lg: 6px; }
.hero { padding: 64px; background: linear-gradient(135deg, var(--accent), #22d3ee); color: white; }
```

In the terminal, CSS doesn't apply, but colours, bold and muted do.

---

## 9. Calling APIs and backends

Handlers run on your machine, so they can call anything. The `http`
module is the HTTP client:

```python
from "http" use get, post

r = get("https://api.github.com/repos/python/cpython",
        headers={"Authorization": "Bearer " + token}, timeout=10)
if r.ok:
    stars = r.json["stargazers_count"]
else:
    toast "GitHub said {r.status}" error
```

`get`, `post`, `put`, `patch`, `delete` and `head` all take:

| Option | Meaning |
| --- | --- |
| `headers={…}` | request headers |
| `json=value` | send JSON (sets `Content-Type`) |
| `form={…}` | send a form (URL-encoded) |
| `params={…}` | add a query string (`?q=…&page=2`) |
| `body="…"` | a raw body (`post(url, body)`) |
| `timeout=30` | seconds |
| `raise_errors=True` | raise `IOError` on network errors and non-2xx |

The response is a dict (fields are also readable as attributes):

| Field | |
| --- | --- |
| `r.status` | HTTP status (0 if there was no answer) |
| `r.ok` | True for 2xx |
| `r.json` | the parsed body when the server sent JSON, else `Nil` |
| `r.body` | the body as text |
| `r.headers` | lower-case header names |
| `r.error` | `Nil`, or why it failed (DNS, timeout, TLS …) |
| `r.url` | the final URL after redirects |

**Calls don't freeze the app.** Inside a handler, an HTTP call waits
for its answer while the app keeps handling everything else: other
clicks, other windows, timers. Show progress with a flag:

```python
users = []
loading = False

fn load():
    global users, loading
    loading = True               # the page shows the spinner right away
    users = get(API + "/users").json
    loading = False              # and the table when the answer arrives

page "/users":
    button "Reload" icon="refresh": load()
    if loading:
        spinner "Loading…"
    else:
        table users
```

**Keep API code in its own file** (`api/users.my`) and return plain data
to the pages. `mymo new` sets a project up that way.

Other backends:

| Need | Use |
| --- | --- |
| SQLite database | `from "sqlite" use open, query, run` |
| Files | `from "io" use read, write, exists, listdir` |
| Shell commands | `from "os" use popen` |
| JSON | `from "json" use encode, decode` |
| Your own REST API for other clients | `mono` (see the README) |

---

## 10. Background work, timers and live updates

```python
spawn: load()                 # start background work now (e.g. at startup)

every 5:                      # every 5 seconds while the app runs
    prices = get(PRICES_URL).json

button "Import":
    progress_note = "Working…"
    sleep 2                   # waits without freezing anything
    progress_note = "Done"
```

Whenever a handler, timer or background task changes something, every
open window updates immediately. The server pushes the change; the
browser doesn't poll. An idle app sends no requests except a tiny
keep-alive every 25 seconds.

---

## 11. Where your app runs

| Command | What you get |
| --- | --- |
| `mymo app.my` | opens in your default browser |
| `mymo app.my --desktop` | its own window (Chrome, Edge or Brave in app mode, without tabs or address bar); closing the window stops the app |
| `mymo app.my --phone` | serves on your Wi-Fi and prints a link; open it on your phone, then Share → Add to Home Screen to install it like an app |
| `mymo app.my --term` | full-screen in the terminal |
| `mymo app.my --serve` | serves on localhost without opening anything |
| `mymo app.my --html` / `--text` | prints the home page's HTML / terminal text and exits |
| `--port 9000` | a different port (the default is 8765; the next free one is used if busy) |
| `--dev` | reload on save (combine with any of the above) |

**Phone mode security.** The app listens on your network. The link
carries a random key, and requests without it are refused. Anyone you
give the link to can use the app while it runs.

**Terminal keys.** ↑/↓ or Tab move between controls, Enter presses,
Space toggles, ←/→ change selects, sliders and tabs, typing edits the
focused input, Esc closes a dialog or quits, `q` quits.

---

## 12. Projects

`mymo new my-app` creates:

```
my-app/
  app.my                 entry point: imports the pages, starts the app
  pages/
    home.my              page "/"
    users.my             page "/users" (loads from an API)
    about.my             page "/about"
  components/
    layout.my            the frame every page shares
  api/
    users.my             HTTP calls
  static/
    app.css              your styles (loaded automatically)
  README.md
```

**Imports** are paths without `.my`:

```python
from "pages/home" use *
from "components/layout" use layout
from "api/users" use fetch_users
```

A path is looked up next to the importing file first, then from the
project folder (where `app.my` is), so any file can import any other
file by its project path. Each file is loaded once, and every file shares
the same app.

**Shared data** that several pages use belongs in its own module (say
`store.my`), next to the functions that change it. Import
the module itself with `use "store"` and read `store.name`: that always
shows the current value. `from "store" use name` copies the value once,
at import time, so use it for functions and states, not for plain
variables that change.

```python
# store.my
place = Nil
fn load(name):
    global place
    place = find_city(name)

# pages/today.my
use "store"
page "/":
    text "Weather in {store.place['name']}" if store.place else "Pick a city"
```

**Static files** in `static/` are served at `/static/…`:
`image "/static/logo.png"`.

**Dev mode.** `mymo app.my --dev` watches every `.my` file and
`static/`. When you save, it checks the file for errors and then restarts
the app in a few milliseconds, and the open page reloads itself. If the
file has a syntax error, the running app keeps working, the terminal
shows the error, and the browser shows a toast.

Data held in variables starts fresh on each restart. Keep anything that
must survive in a file or a SQLite database.

---

## 13. Testing

Pages can be rendered without starting anything:

```python
from "ui" use *
from "pages/home" use *

print(render_text("/", width=60))          # what --term would draw
assert "Hello" in render("/")              # the page's HTML
```

`render_text` is handy for golden-file tests.
`examples/ui_blocks.my` in the MyMo repository shows a test that also
simulates clicks.

---

## 14. Performance

Measured on the multi-page dashboard example (`examples/ui/dashboard.my`,
Apple Silicon, release build):

| | |
| --- | --- |
| Start-up | about 10 ms |
| Full page | under 1 ms to build, about 30 KB with all CSS and JS inlined, no other requests |
| A click | about 0.5 ms on the server, then a patch of about 7 KB |
| Idle | no requests, no CPU |

Why it's fast:
- The server builds HTML directly from your blocks, with no framework
  overhead.
- The browser patches only what changed instead of redrawing.
- Handlers reuse the handler table from the page the user is looking at
  instead of rebuilding it.
- Updates are pushed as they happen instead of polled.

To keep it fast:
- Don't do slow work in page blocks, since they run after every change.
  Load data in handlers, `spawn` or `every`.
- Show large tables a page at a time.

---

## 15. Gotchas

- **Only handler blocks change outer variables by themselves.** Inside a
  normal `fn`, write `global name` first.
- **Inputs need a `state`.** `input name` only works when
  `name = state("")`.
- **`{…}` interpolation is for command arguments.** In ordinary calls
  write `f"…"`: `get(f"{API}/users/{id}")`.
- **Everyone shares the data.** There are no per-user sessions yet, so
  a multi-user app with logins isn't a fit.
- **Windows:** the browser, desktop and phone targets need non-blocking
  sockets, which aren't implemented on Windows yet. `--term` works.
- **`--desktop` needs Chrome, Edge, Brave, Chromium or Arc.** Without one
  it opens a normal browser tab.
- **A file named like a module shadows it.** A `http.my` next to your app
  hides the `http` module.

---

## 16. Plain-call style

Everything above can also be written as ordinary function calls, which
is useful when you build parts of a page in expressions or prefer
explicit nesting:

```python
from "ui" use *

count = state(0)

@page("/")
fn home():
    return card(
        h1("Counter"),
        text(f"Clicked {count} times"),
        row(
            button("-", on=count.dec),
            button("+", on=count.inc, primary=True),
        ),
        title="Demo",
    )

run("Counter")
```

The block `body` of any component is the keyword argument `body=`, so
`card("Title", body=fn)` equals `card "Title": …`. Both styles mix freely.
