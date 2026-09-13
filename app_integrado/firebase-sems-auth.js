const firebaseConfig = {
  apiKey: 'AIzaSyCU1M_zmx5zqqrEc-L0JxujcyOSX0oq5KY',
  authDomain: 'goodwedb.firebaseapp.com',
  databaseURL: 'https://goodwedb-default-rtdb.firebaseio.com',
  projectId: 'goodwedb',
  storageBucket: 'goodwedb.firebasestorage.app',
  messagingSenderId: '738074450770',
  appId: '1:738074450770:web:00a4a82ffec9985025788b',
  measurementId: 'G-W2QN1PG6BN'
};

let auth;
let authSdk;

const firebaseReady = Promise.all([
  import('https://www.gstatic.com/firebasejs/12.17.1/firebase-app.js'),
  import('https://www.gstatic.com/firebasejs/12.17.1/firebase-auth.js')
]).then(([appSdk, loadedAuthSdk]) => {
  const app = appSdk.getApps().length
    ? appSdk.getApp()
    : appSdk.initializeApp(firebaseConfig);

  authSdk = loadedAuthSdk;
  auth = authSdk.getAuth(app);
  auth.useDeviceLanguage();
  return auth;
});

window.semsFirebaseSignIn = async (email, password) => {
  await firebaseReady;
  const credential = await authSdk.signInWithEmailAndPassword(auth, email, password);
  await authSdk.reload(credential.user);

  if (!credential.user.emailVerified) {
    await authSdk.signOut(auth);
    const error = new Error('Email ainda não verificado.');
    error.code = 'auth/email-not-verified';
    throw error;
  }

  return credential.user;
};

window.semsFirebaseSignOut = async () => {
  await firebaseReady;
  await authSdk.signOut(auth);
};

firebaseReady.catch(error => {
  console.error('Não foi possível iniciar o Firebase Authentication do SEMS.', error);
});
