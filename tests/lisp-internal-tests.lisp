(in-package #:tomoe)

(defvar *unit-counter* 0)

(defmacro unit-twice (form)
  `(progn ,form ,form))

(unit-check "macro-backquote" 2
            (progn (setq *unit-counter* 0) (unit-twice (incf *unit-counter*)) *unit-counter*))

(defmacro unit-with-temp (name value &body body)
  `(let ((,name ,value)) ,@body))

(unit-check "macro-splicing" 6 (unit-with-temp x 3 (+ x x)))

(unit-check "handler-case" :caught
            (handler-case (error "x") (error (condition) (declare (ignore condition)) :caught)))
(unit-check "handler-case-value" 4 (handler-case (+ 2 2) (error (condition) condition)))
(unit-check "handler-case-message" t
            (handler-case (error "detail ~A" 7)
              (error (condition) (not (null (search "detail 7" (%condition-message condition)))))))

(unit-check "catch" t
            (%catch (lambda () (error "boom"))
                    (lambda (message) (and (stringp message) (plusp (length message))))))
(unit-check "catch-pass" 5 (%catch (lambda () (+ 2 3)) (lambda (message) message)))

(unit-check "limits" t
            (multiple-value-bind (value condition)
                (ignore-errors (%with-limits 2000 100 (lambda () (loop do nil))))
              (and (null value) (not (null condition))
                   (not (null (search "budget" (%condition-message condition)))))))

(unit-check "limits-spare-the-caller" 7
            (%with-limits nil 200
                          (lambda ()
                            (ignore-errors (%with-limits nil 150 (lambda () (loop do nil))))
                            (+ 3 4))))

(unit-check "limits-charge-the-evaluation-not-the-wall" 7
            (ignore-errors
             (%with-limits nil 50
                           (lambda ()
                             (dotimes (i 3) (unit-sleep 60))
                             (let ((sum 0)) (dotimes (i 5000) (incf sum)))
                             7))))

(unit-check "limits-count-failures" t
            (let ((before (%budget-failures)))
              (ignore-errors (%with-limits 1000 0 (lambda () (loop do nil))))
              (> (%budget-failures) before)))

(unit-check "copy-tree-long" 200000
            (length (copy-tree (make-list 200000 :initial-element 1))))
(unit-check "copy-tree-dotted" '(1 (2 . 3) . 4) (copy-tree '(1 (2 . 3) . 4)))
(unit-check "copy-tree-cycle" t
            (let ((tree (list 1 2)))
              (setf (car tree) tree)
              (not (null (search "nests" (handler-case (copy-tree tree)
                                           (error (condition) (%condition-message condition))))))))
(unit-check "union-cycle" t
            (let ((cycle (list 1 2)))
              (setf (cdr (cdr cycle)) cycle)
              (not (null (search "budget" (handler-case (%with-limits 100000 0 (lambda () (union cycle cycle)))
                                            (error (condition) (%condition-message condition))))))))
(unit-check "reader-depth" t
            (not (null (search "nest" (handler-case
                                          (read-from-string (concatenate 'string (make-string 5000 :initial-element #\()
                                                                         "1"))
                                        (error (condition) (%condition-message condition)))))))

(unit-check "read-source" 2
            (length (%read-source "(+ 1 2) foo" "TOMOE")))
(unit-check "read-source-form" '(+ 1 2)
            (first (%read-source "(+ 1 2) foo" "TOMOE")))

(defparameter *unit-kept* nil)
(setq *unit-kept* (%keep (list 1 2 3)))
(unit-check "keep" '(1 2 3) *unit-kept*)
(%release *unit-kept*)
(setq *unit-kept* nil)

(unit-check "symbol-definition" t (functionp (%symbol-definition 'car)))
(unit-check "set-symbol-definition" 12
            (progn (%set-symbol-definition 'unit-defined (lambda (x) (* x 2)))
                   (funcall 'unit-defined 6)))
(%set-symbol-definition 'unit-defined nil)
(unit-check "unset-symbol-definition" nil (fboundp 'unit-defined))

(unit-check "hash-table" '(2 nil)
            (let ((table (make-hash-table :test #'equal)))
              (setf (gethash "a" table) 2)
              (list (gethash "a" table) (gethash "b" table))))
(unit-check "hash-count" 1
            (let ((table (make-hash-table)))
              (setf (gethash 1 table) 1)
              (hash-table-count table)))
(unit-check "remhash" 0
            (let ((table (make-hash-table)))
              (setf (gethash 1 table) 1)
              (remhash 1 table)
              (hash-table-count table)))

(unit-check "eval" 3 (eval '(+ 1 2)))
(unit-check "prog1" 1 (prog1 1 2 3))
(unit-check "check-type-ok" nil (check-type 5 (integer 1 10)))
(unit-check "check-type-boolean" nil (check-type t boolean))
(unit-check "check-type-and" nil (check-type "x" (and string (satisfies stringp))))
(unit-check "check-type-or" nil (check-type nil (or null function)))
(unit-check "check-type-member" nil (check-type :a (member :a :b)))
(unit-check "check-type-fail" t
            (multiple-value-bind (value condition) (ignore-errors (check-type 99 (integer 1 10)))
              (and (null value) (not (null condition)))))

(unit-check "undefined-function-rejected" t
            (multiple-value-bind (value condition)
                (ignore-errors (%load-source "bad-function"
                                             "(defun bad-source () (nonesuch 1))"
                                             "TOMOE-USER"))
              (and (null value) (not (null condition)))))
(unit-check "undefined-variable-rejected" t
            (multiple-value-bind (value condition)
                (ignore-errors (%load-source "bad-variable"
                                             "(defun bad-variable () missing-name)"
                                             "TOMOE-USER"))
              (and (null value) (not (null condition)))))
