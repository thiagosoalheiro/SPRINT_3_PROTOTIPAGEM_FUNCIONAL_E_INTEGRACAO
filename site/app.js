import { initializeApp } from "https://www.gstatic.com/firebasejs/10.8.0/firebase-app.js";
import { getAuth, onAuthStateChanged } from "https://www.gstatic.com/firebasejs/10.8.0/firebase-auth.js";
import { getDatabase, ref, onValue, set } from "https://www.gstatic.com/firebasejs/10.8.0/firebase-database.js";
 
// Configuração do Firebase
const firebaseConfig = {
  apiKey: "SUA_API_KEY",
  authDomain: "goodwedb.firebaseapp.com",
  databaseURL: "https://goodwedb-default-rtdb.firebaseio.com",
  projectId: "goodwedb",
  storageBucket: "goodwedb.firebasestorage.app",
  messagingSenderId: "SEU_SENDER_ID",
  appId: "SEU_APP_ID"
};
 
// Inicialização
const app = initializeApp(firebaseConfig);
const auth = getAuth(app);
const database = getDatabase(app);
 
// Escuta estado de autenticação
onAuthStateChanged(auth, (user) => {
  if (user) {
    console.log("Usuário logado:", user.email);
    carregarDadosVagas();
  } else {
    console.log("Nenhum usuário logado.");
  }
});
 
// Leitura em tempo real do Realtime Database
function carregarDadosVagas() {
  const vagasRef = ref(database, 'vagas');
  onValue(vagasRef, (snapshot) => {
    const data = snapshot.val();
    if (data) {
      console.log("Dados atualizados das vagas:", data);
    }
  });
}
 
// Configuração dos botões ao carregar o DOM
document.addEventListener('DOMContentLoaded', () => {
  const btnIniciarList = document.querySelectorAll('.btn-iniciar');
  const btnPararList = document.querySelectorAll('.btn-parar');
 
  btnIniciarList.forEach((btn, index) => {
    btn.addEventListener('click', () => {
      const vagaId = `vaga0${index + 1}`;
      console.log(`Iniciando recarga na ${vagaId}`);
      set(ref(database, `vagas/${vagaId}`), {
        status: "OCUPADO",
        soc: 10,
        potencia: 7.4
      });
    });
  });
 
  btnPararList.forEach((btn, index) => {
    btn.addEventListener('click', () => {
      const vagaId = `vaga0${index + 1}`;
      console.log(`Parando recarga na ${vagaId}`);
      set(ref(database, `vagas/${vagaId}`), {
        status: "LIVRE",
        soc: 0,
        potencia: 0.0
      });
    });
  });
});
