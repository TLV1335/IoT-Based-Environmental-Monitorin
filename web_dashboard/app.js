window.onload = () => {
    const app = new IoTDashboard();
    app.start();
};

class IoTDashboard {
    constructor() {
        // UI elements
        this.tempEl = document.getElementById("tempValue");
        this.humidEl = document.getElementById("humidValue");
        this.statusEl = document.getElementById("statusValue");

        // sensor state
        this.temp = 28;
        this.humid = 65;

        // chart data buffer
        this.maxPoints = 10;

        this.initChart();
    }

    /* =========================
       INIT CHART
    ========================= */
    initChart() {
    const tempCtx = document.getElementById("tempChart");
    const humidCtx = document.getElementById("humidChart");
    const dustCtx = document.getElementById("dustChart");

    const options = this.getChartOptions();

    this.tempChart = new Chart(tempCtx, {
        type: "line",
        data: {
            labels: ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"],
            datasets: [{
                label: "Temperature",
                data: [28, 29, 30, 31, 30, 29, 28],
                borderColor: "orange",
                tension: 0.4
            }]
        },
        options
    });

    this.humidChart = new Chart(humidCtx, {
        type: "line",
        data: {
            labels: ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"],
            datasets: [{
                label: "Humidity",
                data: [60, 62, 65, 63, 66, 68, 65],
                borderColor: "skyblue",
                tension: 0.4
            }]
        },
        options
    });

    this.dustChart = new Chart(dustCtx, {
        type: "line",
        data: {
            labels: ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"],
            datasets: [{
                label: "Dust PM2.5",
                data: [20, 25, 18, 30, 28, 35, 22],
                borderColor: "lightgreen",
                tension: 0.4
            }]
        },
        options
    });
}

    /* =========================
       CHART OPTIONS
    ========================= */
    getChartOptions() {
        return {
            responsive: true,
            maintainAspectRatio: false,
            plugins: {
                legend: {
                    labels: {
                        color: "white"
                    }
                }
            },
            scales: {
                x: {
                    ticks: { color: "white" }
                },
                y: {
                    ticks: { color: "white" }
                }
            }
        };
    }

    /* =========================
       SENSOR SIMULATION
    ========================= */
    updateSensor() {
        // random walk (giống IoT thật hơn random cố định)
        this.temp += (Math.random() * 2 - 1);
        this.humid += (Math.random() * 2 - 1);

        // clamp values
        this.temp = this.clamp(this.temp, 25, 40);
        this.humid = this.clamp(this.humid, 50, 100);

        // update UI
        this.tempEl.innerText = this.temp.toFixed(1) + "°C";
        this.humidEl.innerText = this.humid.toFixed(1) + "%";

        this.updateTempColor();
    }

    /* =========================
       COLOR WARNING
    ========================= */
    updateTempColor() {
        if (this.temp > 40) {
            this.tempEl.style.color = "red";
        } else if (this.temp > 32) {
            this.tempEl.style.color = "yellow";
        } else {
            this.tempEl.style.color = "white";
        }
    }

    /* =========================
       CHART UPDATE
    ========================= */
    updateChart() {
    const label = this.getTimeLabel();

    // TEMP
    this.tempChart.data.labels.push(label);
    this.tempChart.data.datasets[0].data.push(this.temp);

    // HUMID
    this.humidChart.data.labels.push(label);
    this.humidChart.data.datasets[0].data.push(this.humid);

    // DUST
    this.dustChart.data.labels.push(label);
    this.dustChart.data.datasets[0].data.push(this.generateDust());

    // LIMIT POINTS
    if (this.tempChart.data.labels.length > this.maxPoints) {
        this.tempChart.data.labels.shift();
        this.humidChart.data.labels.shift();
        this.dustChart.data.labels.shift();

        this.tempChart.data.datasets[0].data.shift();
        this.humidChart.data.datasets[0].data.shift();
        this.dustChart.data.datasets[0].data.shift();
    }

    this.tempChart.update();
    this.humidChart.update();
    this.dustChart.update();
}

    /* =========================
       HELPERS
    ========================= */
    generateDust() {
        return 20 + Math.random() * 30;
    }

    getTimeLabel() {
        const now = new Date();
        return now.getHours() + ":" + now.getMinutes();
    }

    clamp(value, min, max) {
        return Math.min(max, Math.max(min, value));
    }

    /* =========================
       MAIN LOOP
    ========================= */
    start() {
        this.updateSensor();

        setInterval(() => {
            this.updateSensor();
            this.updateChart();
        }, 2000);
    }
}