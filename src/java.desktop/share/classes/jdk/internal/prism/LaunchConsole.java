/*
 * Copyright (c) 2026, MoeRain Software Developments. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 */

package jdk.internal.prism;

import java.awt.BorderLayout;
import java.awt.Color;
import java.awt.Font;
import java.io.BufferedWriter;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.PrintStream;
import java.io.Writer;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicBoolean;
import javax.swing.JButton;
import javax.swing.JFileChooser;
import javax.swing.JFrame;
import javax.swing.JLabel;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JTextArea;
import javax.swing.SwingUtilities;
import javax.swing.Timer;
import javax.swing.WindowConstants;
import javax.swing.border.EmptyBorder;
import javax.swing.filechooser.FileNameExtensionFilter;

/**
 * Built-in PRISM-MCJDK console window.
 *
 * <p>Enabled with {@code -Dprism.launch.console=true}.  It captures
 * {@code System.out}/{@code System.err}, shows the game log and current heap
 * usage, and offers to save the log after the application exits.  A shutdown
 * hook saves the log automatically if the application calls
 * {@code System.exit} or terminates without going through the normal exit
 * path.</p>
 */
public final class LaunchConsole {
    private static final DateTimeFormatter TS = DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss");
    private static final ConcurrentLinkedQueue<String> pending = new ConcurrentLinkedQueue<>();
    private static final AtomicBoolean handled = new AtomicBoolean();

    private static JFrame frame;
    private static JTextArea area;
    private static JLabel status;
    private static Timer memoryTimer;
    private static Timer drainTimer;
    private static PrintStream oldOut;
    private static PrintStream oldErr;
    private static Writer logWriter;
    private static Path tempLog;
    private static Path defaultLog;
    private static Charset charset = StandardCharsets.UTF_8;
    private static long startTime;
    private static volatile boolean finished;

    private LaunchConsole() {
    }

    public static void start() {
        if (frame != null) {
            return;
        }
        try {
            String enc = System.getProperty("stdout.encoding", "UTF-8");
            try {
                charset = Charset.forName(enc);
            } catch (Exception ignored) {
                charset = StandardCharsets.UTF_8;
            }

            tempLog = Files.createTempFile("prism-console-", ".log");
            defaultLog = defaultLogPath();
            logWriter = new BufferedWriter(new OutputStreamWriter(
                    Files.newOutputStream(tempLog, StandardOpenOption.CREATE,
                            StandardOpenOption.TRUNCATE_EXISTING),
                    StandardCharsets.UTF_8));

            oldOut = System.out;
            oldErr = System.err;
            UiOutputStream ui = new UiOutputStream();
            System.setOut(new PrintStream(new TeeOutputStream(oldOut, ui), true, charset));
            System.setErr(new PrintStream(new TeeOutputStream(oldErr, ui), true, charset));

            startTime = System.currentTimeMillis();
            SwingUtilities.invokeAndWait(LaunchConsole::createWindow);
            Runtime.getRuntime().addShutdownHook(new Thread(LaunchConsole::onShutdown,
                    "prism-console-shutdown"));
        } catch (Throwable t) {
            restoreStreams();
            PrintStream err = oldErr != null ? oldErr : System.err;
            if (err != null) {
                t.printStackTrace(err);
            }
        }
    }

    public static void finish(int exitCode) {
        finished = true;
        restoreStreams();
        if (memoryTimer != null) {
            memoryTimer.stop();
        }
        if (drainTimer != null) {
            drainTimer.stop();
        }
        drainPending();
        if (logWriter != null) {
            try {
                logWriter.flush();
            } catch (IOException ignored) {
            }
        }

        if (frame == null) {
            return;
        }
        Runnable task = () -> {
            String state = exitCode == 0
                    ? "\u6E38\u620F\u5DF2\u9000\u51FA"
                    : "\u6E38\u620F\u5F02\u5E38\u9000\u51FA (code " + exitCode + ")";
            status.setText(status.getText() + "   " + state);
            setCloseable();
            if (handled.compareAndSet(false, true)) {
                Path propertyLog = configuredLogPath();
                if (propertyLog != null) {
                    saveTo(propertyLog, false);
                    dispose();
                    return;
                }
                if (!Boolean.getBoolean("prism.console.autoclose")) {
                    showExitDialog(state);
                }
                dispose();
            }
        };
        if (SwingUtilities.isEventDispatchThread()) {
            task.run();
        } else {
            try {
                SwingUtilities.invokeAndWait(task);
            } catch (Exception e) {
                task.run();
            }
        }
    }

    private static void createWindow() {
        frame = new JFrame("PRISM-MCJDK \u63A7\u5236\u53F0");
        frame.setDefaultCloseOperation(WindowConstants.HIDE_ON_CLOSE);

        area = new JTextArea(28, 100);
        area.setEditable(false);
        area.setFont(new Font(Font.MONOSPACED, Font.PLAIN, 12));
        area.setBackground(new Color(18, 18, 20));
        area.setForeground(new Color(210, 220, 225));
        area.setCaretColor(Color.WHITE);

        JScrollPane scroll = new JScrollPane(area);
        JButton saveButton = new JButton("\u4FDD\u5B58\u65E5\u5FD7");
        saveButton.addActionListener(e -> saveLog(null));

        status = new JLabel("\u542F\u52A8\u4E2D...");
        JPanel bottom = new JPanel(new BorderLayout(8, 0));
        bottom.setBorder(new EmptyBorder(6, 8, 6, 8));
        bottom.add(status, BorderLayout.CENTER);
        bottom.add(saveButton, BorderLayout.EAST);

        frame.setLayout(new BorderLayout());
        frame.add(scroll, BorderLayout.CENTER);
        frame.add(bottom, BorderLayout.SOUTH);
        frame.setSize(1000, 640);
        frame.setLocationRelativeTo(null);
        frame.setVisible(true);

        memoryTimer = new Timer(2000, e -> updateStatus());
        memoryTimer.start();
        drainTimer = new Timer(100, e -> drainPending());
        drainTimer.start();
        updateStatus();
    }

    private static void updateStatus() {
        long pid = ProcessHandle.current().pid();
        Runtime rt = Runtime.getRuntime();
        long used = rt.totalMemory() - rt.freeMemory();
        long max = rt.maxMemory();
        long elapsed = (System.currentTimeMillis() - startTime) / 1000;
        status.setText(String.format("PID %d   \u5185\u5B58 %s / %s   \u8FD0\u884C %s",
                pid, formatBytes(used), formatBytes(max), formatDuration(elapsed)));
    }

    private static void drainPending() {
        if (area == null) {
            return;
        }
        for (int i = 0; i < 500; i++) {
            String line = pending.poll();
            if (line == null) {
                break;
            }
            area.append(line);
            area.append("\n");
        }
        if (area.getLineCount() > 6000) {
            try {
                int end = area.getLineEndOffset(1000);
                area.replaceRange("", 0, end);
            } catch (Exception ignored) {
            }
        }
        area.setCaretPosition(area.getDocument().getLength());
    }

    private static void showExitDialog(String state) {
        Object[] options = {"\u4FDD\u5B58\u65E5\u5FD7", "\u76F4\u63A5\u5173\u95ED"};
        int choice = JOptionPane.showOptionDialog(frame,
                state + "\n\u662F\u5426\u4FDD\u5B58\u672C\u6B21\u6E38\u620F\u65E5\u5FD7\uFF1F",
                "PRISM-MCJDK", JOptionPane.YES_NO_OPTION,
                JOptionPane.QUESTION_MESSAGE, null, options, options[0]);
        if (choice == 0) {
            saveLog(null);
        }
    }

    private static void saveLog(Path targetPath) {
        Path target = targetPath;
        if (target == null) {
            JFileChooser chooser = new JFileChooser();
            chooser.setDialogTitle("\u4FDD\u5B58\u6E38\u620F\u65E5\u5FD7");
            chooser.setSelectedFile(new java.io.File(
                    "prism-game-" + LocalDateTime.now().format(TS) + ".log"));
            chooser.setFileFilter(new FileNameExtensionFilter("Log files", "log", "txt"));
            if (chooser.showSaveDialog(frame) != JFileChooser.APPROVE_OPTION) {
                return;
            }
            target = chooser.getSelectedFile().toPath();
        }
        saveTo(target, true);
    }

    private static void saveTo(Path target, boolean showMessage) {
        try {
            if (logWriter != null) {
                logWriter.flush();
            }
            if (target.getParent() != null) {
                Files.createDirectories(target.getParent());
            }
            Files.copy(tempLog, target, StandardCopyOption.REPLACE_EXISTING);
            appendGameLog(target);
            if (showMessage && frame != null) {
                JOptionPane.showMessageDialog(frame,
                        "\u65E5\u5FD7\u5DF2\u4FDD\u5B58\u5230:\n" + target,
                        "PRISM-MCJDK", JOptionPane.INFORMATION_MESSAGE);
            }
        } catch (IOException e) {
            if (showMessage && frame != null) {
                JOptionPane.showMessageDialog(frame,
                        "\u4FDD\u5B58\u65E5\u5FD7\u5931\u8D25:\n" + e,
                        "PRISM-MCJDK", JOptionPane.ERROR_MESSAGE);
            }
        }
    }

    private static void appendGameLog(Path target) {
        Path latest = Paths.get(System.getProperty("user.dir", "."), "logs", "latest.log");
        if (!Files.exists(latest)) {
            return;
        }
        try (OutputStream out = Files.newOutputStream(target, StandardOpenOption.APPEND)) {
            out.write((System.lineSeparator() + "===== latest.log =====" + System.lineSeparator())
                    .getBytes(StandardCharsets.UTF_8));
            Files.copy(latest, out);
        } catch (IOException ignored) {
        }
    }

    private static void onShutdown() {
        if (handled.get()) {
            return;
        }
        try {
            if (logWriter != null) {
                logWriter.flush();
            }
            if (tempLog != null && defaultLog != null) {
                Files.createDirectories(defaultLog.getParent());
                Files.copy(tempLog, defaultLog, StandardCopyOption.REPLACE_EXISTING);
                PrintStream err = oldErr != null ? oldErr : System.err;
                if (err != null) {
                    err.println("[PRISM] console log: " + defaultLog);
                }
            }
        } catch (IOException ignored) {
        }
    }

    private static void restoreStreams() {
        if (oldOut != null) {
            System.setOut(oldOut);
        }
        if (oldErr != null) {
            System.setErr(oldErr);
        }
    }

    private static void setCloseable() {
        if (frame != null) {
            frame.setDefaultCloseOperation(WindowConstants.DISPOSE_ON_CLOSE);
        }
    }

    private static void dispose() {
        if (frame != null) {
            frame.dispose();
            frame = null;
        }
    }

    private static Path configuredLogPath() {
        String value = System.getProperty("prism.console.log");
        if (value == null || value.isBlank()) {
            return null;
        }
        return Paths.get(value);
    }

    private static Path defaultLogPath() {
        String local = System.getenv("LOCALAPPDATA");
        Path base = local != null && !local.isBlank()
                ? Paths.get(local, "PRISM-MCJDK")
                : Paths.get(System.getProperty("user.home", "."), ".prism-mcjd");
        return base.resolve("logs").resolve("game-" + LocalDateTime.now().format(TS) + ".log");
    }

    private static String formatBytes(long bytes) {
        if (bytes >= 1024L * 1024 * 1024) {
            return String.format("%.2f GB", bytes / (1024.0 * 1024 * 1024));
        }
        if (bytes >= 1024L * 1024) {
            return String.format("%.1f MB", bytes / (1024.0 * 1024));
        }
        return bytes + " B";
    }

    private static String formatDuration(long seconds) {
        long h = seconds / 3600;
        long m = (seconds % 3600) / 60;
        long s = seconds % 60;
        return String.format("%02d:%02d:%02d", h, m, s);
    }

    private static void appendLine(String line) {
        pending.add(line);
        synchronized (LaunchConsole.class) {
            if (logWriter != null) {
                try {
                    logWriter.write(line);
                    logWriter.write(System.lineSeparator());
                    logWriter.flush();
                } catch (IOException ignored) {
                }
            }
        }
    }

    private static final class TeeOutputStream extends OutputStream {
        private final OutputStream first;
        private final OutputStream second;

        TeeOutputStream(OutputStream first, OutputStream second) {
            this.first = first;
            this.second = second;
        }

        @Override
        public void write(int b) throws IOException {
            try {
                if (first != null) {
                    first.write(b);
                }
            } finally {
                if (second != null) {
                    second.write(b);
                }
            }
        }

        @Override
        public void write(byte[] b, int off, int len) throws IOException {
            try {
                if (first != null) {
                    first.write(b, off, len);
                }
            } finally {
                if (second != null) {
                    second.write(b, off, len);
                }
            }
        }

        @Override
        public void flush() throws IOException {
            if (first != null) {
                first.flush();
            }
            if (second != null) {
                second.flush();
            }
        }
    }

    private static final class UiOutputStream extends OutputStream {
        private final ByteArrayOutputStream buffer = new ByteArrayOutputStream();

        @Override
        public synchronized void write(int b) {
            if (b == '\n') {
                emit();
            } else if (b != '\r') {
                buffer.write(b);
            }
        }

        @Override
        public synchronized void write(byte[] b, int off, int len) {
            for (int i = 0; i < len; i++) {
                write(b[off + i] & 0xff);
            }
        }

        @Override
        public synchronized void flush() {
            if (buffer.size() > 0) {
                emit();
            }
        }

        private void emit() {
            String line = buffer.toString(charset);
            buffer.reset();
            appendLine(line);
        }
    }
}
