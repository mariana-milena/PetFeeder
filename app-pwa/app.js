(() => {
  "use strict";

  const MQTT_URL = "wss://broker.hivemq.com:8884/mqtt";

  const TOPIC_CMD_IMEDIATO = "petfeeder/cmd/imediato";
  const TOPIC_CMD_AGENDAR = "petfeeder/cmd/agendar";
  const TOPIC_STATUS_AGENDA = "petfeeder/status/agenda";
  const TOPIC_STATUS_DEVICE = "petfeeder/status/dispositivo";
  const TOPIC_STATUS_SENSOR = "petfeeder/alerta/vazio";

  const appConnStatus = document.getElementById("app-conn-status");
  const appConnText = document.getElementById("app-conn-text");
  const deviceConnStatus = document.getElementById("device-conn-status");
  const deviceConnText = document.getElementById("device-conn-text");
  const sensorStatus = document.getElementById("sensor-status");
  const sensorText = document.getElementById("sensor-text");
  const btnDispense = document.getElementById("btn-dispense");
  const dispenseFeedback = document.getElementById("dispense-feedback");
  const btnNew = document.getElementById("btn-new");
  const btnCancelNew = document.getElementById("btn-cancel-new");
  const newScheduleForm = document.getElementById("new-schedule-form");
  const inputTime = document.getElementById("input-time");
  const scheduleList = document.getElementById("schedule-list");
  const scheduleEmpty = document.getElementById("schedule-empty");
  const scheduleFooter = document.getElementById("schedule-footer");

  const clientId = "petfeeder_pwa_" + Math.random().toString(16).slice(2, 10);
  const client = mqtt.connect(MQTT_URL, {
    clientId,
    clean: true,
    reconnectPeriod: 3000,
  });

  // Status do APP: link deste navegador/celular com o broker publico.
  // Fica "Conectado" assim que o WebSocket abre, independente do ESP32.
  const APP_CONN_LABELS = {
    connecting: "App: conectando...",
    connected: "App: conectado",
    reconnecting: "App: reconectando...",
    offline: "App: desconectado",
    error: "App: erro de conexão",
  };

  function setAppConnState(state) {
    appConnStatus.dataset.state = state;
    appConnText.textContent = APP_CONN_LABELS[state] || state;
  }

  // Status do comedouro: vem do ESP32 via petfeeder/status/dispositivo.
  // "online" e publicado pelo firmware ao conectar e "offline" e publicado
  // pelo proprio broker (Last Will) se o ESP32 cair sem avisar.
  const DEVICE_LABELS = {
    unknown: "Comedouro: ?",
    online: "Comedouro: online",
    offline: "Comedouro: offline",
  };

  function setDeviceState(state) {
    deviceConnStatus.dataset.state = state;
    deviceConnText.textContent = DEVICE_LABELS[state] || state;
  }

  // Status do sensor de nivel: vem do ESP32 via petfeeder/alerta/vazio
  // (retained). "vazio"/"ok" so mudam quando o firmware detecta uma
  // transicao real na leitura do sensor — nao e um valor continuo.
  const SENSOR_LABELS = {
    unknown: "Ração: ?",
    ok: "Ração: ok",
    vazio: "Ração: vazia",
  };

  function setSensorState(state) {
    sensorStatus.dataset.state = state;
    sensorText.textContent = SENSOR_LABELS[state] || state;
  }

  setAppConnState("connecting");
  setDeviceState("unknown");
  setSensorState("unknown");

  client.on("connect", () => {
    setAppConnState("connected");
    client.subscribe(TOPIC_STATUS_AGENDA);
    client.subscribe(TOPIC_STATUS_DEVICE);
    client.subscribe(TOPIC_STATUS_SENSOR);
  });

  client.on("reconnect", () => setAppConnState("reconnecting"));
  client.on("close", () => setAppConnState("offline"));
  client.on("offline", () => setAppConnState("offline"));
  client.on("error", (err) => {
    console.error("[MQTT] erro:", err);
    setAppConnState("error");
  });

  client.on("message", (topic, payload) => {
    if (topic === TOPIC_STATUS_AGENDA) {
      renderSchedule(parseAgendaStatus(payload.toString()));
    } else if (topic === TOPIC_STATUS_DEVICE) {
      const value = payload.toString();
      setDeviceState(value === "online" ? "online" : "offline");
    } else if (topic === TOPIC_STATUS_SENSOR) {
      const value = payload.toString();
      setSensorState(value === "vazio" ? "vazio" : "ok");
    }
  });

  // Formato publicado pelo firmware: "slot,HH:MM,ativo;slot,HH:MM,ativo;..."
  // Um slot com HH:MM vazio significa que nunca foi configurado.
  function parseAgendaStatus(raw) {
    return raw
      .split(";")
      .filter((entry) => entry.length > 0)
      .map((entry) => {
        const [slotStr, hora, ativoStr] = entry.split(",");
        return {
          slot: Number(slotStr),
          hora: hora || "",
          ativo: ativoStr === "1",
        };
      })
      .filter((item) => item.hora !== "")
      .sort((a, b) => a.hora.localeCompare(b.hora));
  }

  function renderSchedule(items) {
    scheduleList.innerHTML = "";

    if (items.length === 0) {
      scheduleEmpty.classList.remove("hidden");
      scheduleFooter.classList.add("hidden");
      return;
    }

    scheduleEmpty.classList.add("hidden");

    items.forEach((item) => scheduleList.appendChild(buildScheduleItem(item)));

    const ativos = items.filter((item) => item.ativo).length;
    scheduleFooter.textContent = `${ativos} agendamento${ativos === 1 ? "" : "s"} ativo${
      ativos === 1 ? "" : "s"
    }`;
    scheduleFooter.classList.remove("hidden");
  }

  function buildScheduleItem(item) {
    const li = document.createElement("li");
    li.className = "schedule-row";
    li.dataset.active = String(item.ativo);

    const switchLabel = document.createElement("label");
    switchLabel.className = "switch";

    const switchInput = document.createElement("input");
    switchInput.type = "checkbox";
    switchInput.checked = item.ativo;
    switchInput.setAttribute(
      "aria-label",
      `Ativar ou desativar agendamento das ${item.hora}`
    );
    switchInput.addEventListener("change", () => {
      const cmd = switchInput.checked ? "ON" : "OFF";
      client.publish(TOPIC_CMD_AGENDAR, `${cmd}:${item.slot}`);
    });

    const switchTrack = document.createElement("span");
    switchTrack.className = "switch-track";

    switchLabel.appendChild(switchInput);
    switchLabel.appendChild(switchTrack);

    const timeSpan = document.createElement("span");
    timeSpan.className = "row-time";
    timeSpan.textContent = item.hora;

    const removeBtn = document.createElement("button");
    removeBtn.type = "button";
    removeBtn.className = "row-remove";
    removeBtn.innerHTML = "&times;";
    removeBtn.setAttribute("aria-label", `Remover agendamento das ${item.hora}`);
    removeBtn.addEventListener("click", () => {
      client.publish(TOPIC_CMD_AGENDAR, `DEL:${item.slot}`);
    });

    li.appendChild(switchLabel);
    li.appendChild(timeSpan);
    li.appendChild(removeBtn);

    return li;
  }

  // 5000ms casa com DEF_AUGER_RUN_TIME_MS do firmware, ou seja, o botao fica
  // desabilitado pelo tempo real de giro do motor, nao um numero arbitrario.
  const AUGER_RUN_MS = 5000;

  btnDispense.addEventListener("click", () => {
    client.publish(TOPIC_CMD_IMEDIATO, "1");
    btnDispense.disabled = true;
    dispenseFeedback.textContent = "Dispensando...";

    setTimeout(() => {
      btnDispense.disabled = false;
      dispenseFeedback.textContent = "Ração dispensada";

      setTimeout(() => {
        dispenseFeedback.textContent = "";
      }, 1800);
    }, AUGER_RUN_MS);
  });

  btnNew.addEventListener("click", () => {
    newScheduleForm.classList.remove("hidden");
    btnNew.classList.add("hidden");
    inputTime.focus();
  });

  btnCancelNew.addEventListener("click", () => {
    newScheduleForm.reset();
    newScheduleForm.classList.add("hidden");
    btnNew.classList.remove("hidden");
  });

  newScheduleForm.addEventListener("submit", (event) => {
    event.preventDefault();

    if (!inputTime.value) {
      return;
    }

    client.publish(TOPIC_CMD_AGENDAR, inputTime.value);
    newScheduleForm.reset();
    newScheduleForm.classList.add("hidden");
    btnNew.classList.remove("hidden");
  });

  if ("serviceWorker" in navigator) {
    window.addEventListener("load", () => {
      navigator.serviceWorker.register("service-worker.js").catch((err) => {
        console.error("[SW] falha ao registrar:", err);
      });
    });
  }
})();
