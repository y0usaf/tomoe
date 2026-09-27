(in-package #:tomoe)

(sb-alien:define-alien-routine ("tomoe_nvml_count" %nvml-count) sb-alien:unsigned)
(sb-alien:define-alien-routine ("tomoe_nvml_sample" %nvml-sample) sb-alien:int
  (index sb-alien:unsigned) (busy sb-alien:unsigned :out) (used sb-alien:unsigned :out)
  (total sb-alien:unsigned :out) (celsius sb-alien:unsigned :out))

(defun %system-lines (path)
  (ignore-errors
   (with-open-file (in path)
     (loop for line = (read-line in nil) while line collect line))))

(defun %system-integer (path)
  (let ((line (first (%system-lines path))))
    (and line (parse-integer line :junk-allowed t))))

(defun %system-paths (pattern)
  (sort (mapcar #'namestring (directory pattern :resolve-symlinks nil)) #'string<))

(defun %cpu-temperature ()
  "Celsius from the first hwmon named k10temp, coretemp or zenpower, as sysinfo.sh reads it."
  (dolist (hwmon (%system-paths "/sys/class/hwmon/hwmon*/"))
    (let ((want (cdr (assoc (first (%system-lines (concatenate 'string hwmon "name")))
                            '(("k10temp" . "Tctl") ("coretemp" . "Package id 0") ("zenpower" . "Tdie"))
                            :test #'equal))))
      (when want
        (let ((milli (or (loop for label in (%system-paths (concatenate 'string hwmon "temp*_label"))
                               when (equal (first (%system-lines label)) want)
                                 return (%system-integer (concatenate 'string (subseq label 0 (- (length label) 5)) "input")))
                         (%system-integer (concatenate 'string hwmon "temp1_input")))))
          (when milli (return (floor milli 1000))))))))

(defun %gpu-samples ()
  "Each NVIDIA GPU through NVML when its driver is loaded, then each DRM card reporting busy time."
  (append
   (when (probe-file "/proc/driver/nvidia/version")
     (loop for index below (%nvml-count)
           for (ok busy used total celsius) = (multiple-value-list (%nvml-sample index))
           when (= ok 1)
             collect (list :name (format nil "nvidia~D" index) :busy busy :vram-used used
                           :vram-total total :temperature celsius)))
   (loop for card in (%system-paths "/sys/class/drm/card*/")
         for name = (car (last (pathname-directory card)))
         for device = (concatenate 'string card "device/")
         for busy = (and (> (length name) 4) (every #'digit-char-p (subseq name 4))
                         (%system-integer (concatenate 'string device "gpu_busy_percent")))
         when busy
           collect (list :name name :busy busy
                         :vram-used (floor (or (%system-integer (concatenate 'string device "mem_info_vram_used")) 0) 1048576)
                         :vram-total (floor (or (%system-integer (concatenate 'string device "mem_info_vram_total")) 0) 1048576)
                         :temperature (let ((input (first (%system-paths (concatenate 'string device "hwmon/hwmon*/temp1_input")))))
                                        (and input (floor (%system-integer input) 1000)))))))

(defun sample-system ()
  (let* ((stat (first (%system-lines "/proc/stat")))
         (jiffies (loop with start = 0
                        for digit = (position-if #'digit-char-p stat :start start)
                        while digit
                        collect (multiple-value-bind (value end) (parse-integer stat :start digit :junk-allowed t)
                                  (setf start end)
                                  value)))
         (memory (loop for line in (%system-lines "/proc/meminfo")
                       for colon = (position #\: line)
                       for key = (and colon (cdr (assoc (subseq line 0 colon)
                                                        '(("MemTotal" . :total) ("MemAvailable" . :available))
                                                        :test #'equal)))
                       when key collect key and collect (parse-integer line :start (1+ colon) :junk-allowed t))))
    (list :cpu-total (reduce #'+ jiffies :end (min 10 (length jiffies)))
          :cpu-idle (+ (or (fourth jiffies) 0) (or (fifth jiffies) 0))
          :cpu-temperature (%cpu-temperature)
          :memory-total (getf memory :total 0)
          :memory-available (getf memory :available 0)
          :gpus (%gpu-samples))))

(defun system-read-p (runtime)
  (some (lambda (mounted) (member :system (spec-reads (mounted-spec mounted))))
        (runtime-mounts runtime)))

(defun system-wait-milliseconds (runtime maximum)
  (if (system-read-p runtime)
      (min maximum (max 0 (ceiling (* 1000 (- (runtime-system-due runtime) (get-internal-real-time)))
                                   internal-time-units-per-second)))
      maximum))

(defun service-system (runtime)
  "Sample once a second while a mounted extension reads :SYSTEM."
  (when (and (runtime-running runtime) (not *stop-requested*) (system-read-p runtime)
             (>= (get-internal-real-time) (runtime-system-due runtime)))
    (setf (runtime-system-due runtime) (+ (get-internal-real-time) internal-time-units-per-second))
    (dispatch-event runtime (list :type :system :system (sample-system)))))
