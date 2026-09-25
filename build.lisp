(require :sb-posix)
(require :sb-bsd-sockets)

(dolist (name '("api" "json" "native" "ui" "dialogs" "runtime" "rules" "timers" "watches" "executions" "processes" "control" "ipc-transport" "ipc" "notifications" "mpris" "battery" "network" "tray" "main"))
  (multiple-value-bind (output warnings failure)
      (compile-file (format nil "src/~A.lisp" name) :output-file (format nil "build/~A.fasl" name))
    (declare (ignore warnings))
    (when failure (error "Compilation failed: ~A" name))
    (load output)))

(let ((tomoe::*definitions* nil) (tomoe::*source* "builtins/desktop.lisp"))
  (multiple-value-bind (output warnings failure)
      (compile-file "builtins/desktop.lisp" :output-file "build/desktop.fasl")
    (declare (ignore output warnings))
    (when failure (error "Built-in policy compilation failed."))))

(sb-ext:save-lisp-and-die "build/tomoe" :executable t
                         :toplevel #'tomoe::main :save-runtime-options t)
