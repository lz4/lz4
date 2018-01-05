Vagrant.configure('2') do |config|
  config.vm.box = 'velocity42/xenial64'
  config.vm.box_version = '1.1.0'
  config.vm.provision 'shell', privileged: false, inline: <<SCRIPT

  set -e
  set -x
  set -o pipefail

  # Install apt source for yarn
  curl -sSL https://dl.yarnpkg.com/debian/pubkey.gpg | sudo apt-key add -
  echo "deb https://dl.yarnpkg.com/debian/ stable main" | sudo tee /etc/apt/sources.list.d/yarn.list

  # Install apt source for node
  curl -sSL https://deb.nodesource.com/setup_8.x | sudo bash

  # Install node and yarn, and other dependencies
  # See https://github.com/GoogleChrome/puppeteer/blob/master/docs/troubleshooting.md
  sudo apt-get -y install \
    nodejs \
    yarn \
    gconf-service \
    libasound2 \
    libatk1.0-0 \
    libc6 \
    libcairo2 \
    libcups2 \
    libdbus-1-3 \
    libexpat1 \
    libfontconfig1 \
    libgcc1 \
    libgconf-2-4 \
    libgdk-pixbuf2.0-0 \
    libglib2.0-0 \
    libgtk-3-0 \
    libnspr4 \
    libpango-1.0-0 \
    libpangocairo-1.0-0 \
    libstdc++6 \
    libx11-6 \
    libx11-xcb1 \
    libxcb1 \
    libxcomposite1 \
    libxcursor1 \
    libxdamage1 \
    libxext6 \
    libxfixes3 \
    libxi6 \
    libxrandr2 \
    libxrender1 \
    libxss1 \
    libxtst6 \
    ca-certificates \
    fonts-liberation \
    libappindicator1 \
    libnss3 \
    lsb-release \
    xdg-utils \
    wget

  # Install puppeteer
  sudo yarn global add puppeteer@0.13

  # Not clear why this is needed, perhaps a bug in yarn?
  sudo chmod a+r /usr/local/share/.config/yarn/global/node_modules/puppeteer/.local-chromium/linux-515411/chrome-linux/*
  sudo chmod a+x /usr/local/share/.config/yarn/global/node_modules/puppeteer/.local-chromium/linux-515411/chrome-linux/chrome

  # Install emscripten
  curl -sSL https://s3.amazonaws.com/mozilla-games/emscripten/releases/emsdk-portable.tar.gz \
      | tar xz
  (cd emsdk-portable && \
    ./emsdk install emscripten-1.37.22 && \
    ./emsdk install clang-e1.37.22-64bit && \
    ./emsdk activate emscripten-1.37.22 && \
    ./emsdk activate clang-e1.37.22-64bit)

  # Create chrome headless helper script
  cat > ~/chrome-headless-wrapper.sh <<EOF
#!/usr/bin/node
const puppeteer = require('/usr/local/share/.config/yarn/global/node_modules/puppeteer');

(async() => {
  const url = process.argv.find(arg => arg.match(/^http/))
  const browser = await puppeteer.launch();
  console.log(await browser.version());
  const page = await browser.newPage();
  await page.goto(url, {waitUntil: 'domcontentloaded'});
  await new Promise(resolve => setTimeout(resolve, 100));
  await page.evaluate(() => true);
  await browser.close();
  process.exit(0);
})();
EOF
  chmod +x ~/chrome-headless-wrapper.sh

  # Configure shell
  echo 'source ~/emsdk-portable/emsdk_env.sh' >> ~/.profile
  echo 'export EMRUN_FLAGS=--browser=/home/vagrant/chrome-headless-wrapper.sh' >> ~/.profile
SCRIPT
end
