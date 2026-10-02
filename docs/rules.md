# Window rules

Rules attach properties to windows that match them. Policies such as the
shipped window manager read those properties.

`window-rule` declares a named, owned rule. `:app-id` must equal the window's
app id and `:title` must occur in its title, both case-sensitive; anything
richer goes in the `:match` predicate. All supplied matchers must match.
Omitting them matches every known window.

```lisp
(define-extension "editor-rules" (:reads () :state nil)
    (snapshot state event)
  (declare (ignore snapshot event))
  (values state
    (list
      (window-rule :editor :app-id "org.example.Editor"
        :title "Project"
        :properties '((:workspace . 2) (:border . "#89b4fa"))
        :state '(:ticks 0)
        :apply (lambda (window snapshot local event)
          (declare (ignore snapshot))
          (when (eq (getf event :type) :timer)
            (incf (getf local :ticks)))
          (values local
                  (list (place (getf window :id) 80 80 960 640)
                        (interval :refresh 1000))
                  nil))))
    nil))
```

The full form is `(window-rule NAME &key app-id title match properties reads
state apply)`. `NAME` is a keyword local to its owner. `:properties` is an alist
of copied data; later matching declarations replace equal keys, including an
explicit `nil` value. Matcher and callback fields are excluded. Properties have
no built-in meaning: a layout can read `:rules` and use `rules-for` to interpret
them. The shipped WM consumes workspace/fullscreen/focus at admission; a border
property still requires a rendering policy that consumes it.

`:match` receives `(window snapshot)` and returns a truth value. Declare its
context dependencies in the rule's `:reads`. `:apply` receives
`(window snapshot state event)` and returns new private state, its complete
owned effects, and one-shot commands. Its window record includes `:properties`,
the same merged alist returned by `rules-for`. Applications implicitly read
`:windows` and `:rules`; additional subscriptions such as `:outputs` or `:key`
belong in the rule's `:reads`.

Ordinary extension reducers run before rule applications. Applications then
refine their effects in declaration order. Each matching owner/rule/window
combination has independent state and its own names for timers, file watches, async execution,
services, and bindings. Ordinary updates preserve that identity. New matches
receive `:mount`; successful source replacement reapplies to existing windows
with fresh rule state. Rule events also contain `:rule`, `:window`, and `:parent`.
Timer, file watch, and execution events stay private to their instance; key events
target its binding owner, and button/request events target its window.
`inspect` lists the internal owners and state under `:rule-instances`.

Omitting a rule, losing its match, withdrawing its window, or unmounting its owner
withdraws the entire instance and cancels its owned resources. Session run-once
processes retain their documented shutdown lifetime. Predicates and applications
run inside the candidate transaction: callback errors, dependency cycles, and
native rejection preserve the accepted rules and resources. External window
destruction still removes dead scopes even if policy evaluation fails.
An xdg buffer detach keeps the same rule generation, private state and resource
leases. Applications can read `:buffered` to suspend work that needs content;
the shipped drag policy ends its interactive grab when that buffer disappears.
Applications may return one-shot commands on `:mount` as well as the ordinary
input/timer/watch/execution/request events; commands run only after successful
settlement and only while their originating scope still exists.

Keep predicates and reducers pure apart from their returned values. They can
run more than once during dependency settlement. Nested rules are allowed and
are discovered in later reconciliation passes; settlement permits 16 rounds
and at most 4096 matches. Callback evaluation and pattern work are bounded.
These limits reject a candidate instead of leaving partially installed rules.
