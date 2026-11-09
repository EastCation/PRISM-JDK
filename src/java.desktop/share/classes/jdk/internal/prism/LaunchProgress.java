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
import java.awt.Font;
import javax.swing.JFrame;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.JProgressBar;
import javax.swing.SwingConstants;
import javax.swing.SwingUtilities;
import javax.swing.Timer;
import javax.swing.WindowConstants;
import javax.swing.border.EmptyBorder;

/**
 * Built-in PRISM-MCJDK launch progress window.
 *
 * <p>The java launcher calls {@link #start()} for Minecraft client launches
 * before invoking the application main class.  The window is intentionally
 * not closable and disposes itself when the progress animation completes.</p>
 */
public final class LaunchProgress {
    private static final String[] STEPS = {
        "\u68C0\u67E5 Profile \u7F13\u5B58...",
        "\u68C0\u67E5 Native Cache...",
        "\u51C6\u5907 JIT \u53C2\u6570...",
        "\u542F\u52A8\u6E38\u620F..."
    };

    private static JFrame frame;
    private static JProgressBar bar;
    private static JLabel label;
    private static Timer timer;
    private static int value;

    private LaunchProgress() {
    }

    public static void start() {
        SwingUtilities.invokeLater(() -> {
            if (frame != null) {
                return;
            }
            frame = new JFrame("PRISM-MCJDK");
            frame.setDefaultCloseOperation(WindowConstants.DO_NOTHING_ON_CLOSE);
            frame.setResizable(false);
            frame.setAlwaysOnTop(true);

            label = new JLabel("\u6B63\u5728\u4F18\u5316 Java...", SwingConstants.CENTER);
            label.setFont(label.getFont().deriveFont(Font.BOLD, 15f));
            bar = new JProgressBar(0, 100);
            bar.setStringPainted(true);

            JPanel panel = new JPanel(new BorderLayout(10, 10));
            panel.setBorder(new EmptyBorder(18, 24, 18, 24));
            panel.add(label, BorderLayout.NORTH);
            panel.add(bar, BorderLayout.CENTER);
            frame.setContentPane(panel);
            frame.setSize(430, 150);
            frame.setLocationRelativeTo(null);
            frame.setVisible(true);

            value = 0;
            timer = new Timer(35, e -> tick());
            timer.start();
        });
    }

    private static void tick() {
        value += 3;
        if (value >= 100) {
            value = 100;
            bar.setValue(100);
            label.setText("\u4F18\u5316\u5B8C\u6210\uFF0C\u6B63\u5728\u542F\u52A8\u6E38\u620F...");
            timer.stop();
            Timer close = new Timer(300, e -> stop());
            close.setRepeats(false);
            close.start();
        } else {
            bar.setValue(value);
            int idx = Math.min(STEPS.length - 1, value / 25);
            label.setText(STEPS[idx]);
        }
    }

    public static void stop() {
        SwingUtilities.invokeLater(() -> {
            if (timer != null) {
                timer.stop();
                timer = null;
            }
            if (frame != null) {
                frame.dispose();
                frame = null;
            }
        });
    }
}
