# WebRX — écouter un récepteur OpenWebRX

**Français** · [English](../en/webrx.md)

WebRX relie la montre à l'un de tes récepteurs **OpenWebRX** ou **OpenWebRX+** par le Wi-Fi
(`https://` ou `http://`), joue l'audio démodulé sur le haut-parleur de la montre et affiche la
fréquence, un S-mètre et une petite waterfall.

![WebRX et les autres apps radio](../img/radio-fr.png)

## Réglages (une fois, depuis le téléphone)

1. Ouvrir **WebRX** sur la montre et toucher **Configurer** (plus tard : toucher le nom du
   profil en haut → *Wi-Fi et serveur*). Le Bluetooth se met en pause ; la montre crée le
   réseau Wi-Fi **T-Watch-OTA** (même réseau et même mot de passe que pour la mise à jour).
2. Sur le téléphone, rejoindre ce réseau et ouvrir **http://192.168.4.1**.
3. Saisir jusqu'à **3 réseaux Wi-Fi** (par ordre de préférence ; les réseaux visibles sont
   proposés) et jusqu'à **4 serveurs OpenWebRX** (nom facultatif + adresse). Les mots de passe
   enregistrés ne sont pas affichés : laisser le champ vide pour les garder.
4. **Enregistrer** : la montre redémarre directement dans WebRX.

![Page de réglages](../img/webrx-setup-fr.png)

Adresse du serveur : celle qu'on tape dans un navigateur, par exemple `https://sdr.exemple.fr/`
ou `http://192.168.1.20:8073/`. Un serveur derrière un proxy dans un sous-dossier fonctionne
(`https://exemple.fr/owrx/`). Si le proxy demande un mot de passe, l'écrire dans l'adresse :
`https://utilisateur:motdepasse@exemple.fr/`. Les certificats HTTPS sont vérifiés auprès des
autorités publiques habituelles (Let's Encrypt, etc.) ; un certificat auto-signé est refusé.

## Utilisation

| Où | Action |
|---|---|
| Barre du haut, nom du profil | feuille **Récepteur** : choix du **serveur** (s'il y en a plusieurs) et du **profil** (bande) |
| Barre du haut, haut-parleur | feuille **Son** : volume, squelch, tonalité |
| Fréquence | toucher un chiffre : il devient le pas des boutons − / + (appui long = répétition) |
| Nom sous la fréquence | **signets** de la bande (ceux du serveur) |
| Pastille du mode | modes **du serveur**, avec leur bande passante (modes ajoutés compris) |
| Waterfall | toucher : écouter là (accroche un signet proche) ; zoom ×1 / ×4 / ×16 dans le coin |
| ■ / ▶ | arrêter / reprendre l'écoute (Wi-Fi coupé à l'arrêt) |
| Bouton latéral | écran éteint, **l'écoute continue** ; un nouvel appui y revient |
| Glisser → ou ‹ | quitter l'app : l'écoute s'arrête, le Bluetooth revient |

- Chaque écoute démarre à **10 % de volume**.
- **Squelch** : coupe le son tant que le signal reste sous le seuil (appliqué par le serveur).
  Tout à gauche : coupé. Le niveau du signal est affiché sous le curseur.
- **Tonalité** (5 paliers, retenue) : *Neutre*, *Claire*, *Voix*, *Douce* (par défaut),
  *Sourde*. Vers la droite : moins d'aigus et de souffle FM, plus de corps aux voix sur le petit
  haut-parleur.
- L'écran reste allumé pendant l'écoute ; le bouton l'éteint.
- La montre retient le serveur choisi et, pour chaque serveur, la fréquence, le mode, le
  squelch et le profil choisi sur la montre. Sur un serveur partagé, un profil n'est
  re-sélectionné que si tu l'as choisi toi-même.

## Messages

| Message | Signification |
|---|---|
| *Aucun réseau connu à portée* | aucun des réseaux Wi-Fi enregistrés n'est visible |
| *Mot de passe Wi-Fi refusé* / *Connexion Wi-Fi impossible (raison N)* | vérifier le réseau sur la page de réglages |
| *Serveur injoignable ou certificat refusé* | adresse erronée, serveur arrêté ou certificat auto-signé |
| *Pas de WebSocket OpenWebRX à cette adresse* (code HTTP) | l'adresse ne mène pas à OpenWebRX (chemin ?) |
| *Refusé par le serveur* + texte | complet (*Too many clients*) ou adresse bannie (*Client address banned*, protection anti-robot d'OpenWebRX+) |
| *Connexion perdue, reprise…* | nouvel essai après 2, 10, 20 et 40 s, puis toucher pour réessayer |

Chaque coupure est aussi notée dans le journal de *Réglages › À propos* : cause, trames et
octets reçus, durée, RSSI du Wi-Fi, type du dernier message et, sur erreur de lecture, les
codes du transport et de TLS.

## Consommation

Pendant l'écoute : Wi-Fi sans modem sleep (le serveur envoie audio et spectre en continu), CPU
plafonné à 160 MHz (80 MHz écran éteint), écran allumé. Compter plusieurs dizaines de mA :
l'autonomie d'écoute se compte en heures, pas en jours. Quitter l'app coupe complètement le
Wi-Fi.

Fonctionnement interne : [architecture](architecture.md).
