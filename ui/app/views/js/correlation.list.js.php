<?php declare(strict_types = 0);
/*
** Zabbix
** Copyright (C) 2001-2023 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/


/**
 * @var CView $this
 */
?>

<script>
	const view = new class {

		init() {
			this.#initActions();
		}

		/**
		 * Creates the event listeners for create, edit, delete, enable, disable single and mass operations.
		 */
		#initActions() {
			document.getElementById('js-create').addEventListener('click', () => this.#edit());
			document.getElementById('js-massdelete').addEventListener('click',
				(e) => this.#delete(e.target, Object.keys(chkbxRange.getSelectedIds()))
			);
			document.getElementById('js-massenable').addEventListener('click',
				(e) => this.#enable(e.target, Object.keys(chkbxRange.getSelectedIds()), true)
			);
			document.getElementById('js-massdisable').addEventListener('click',
				(e) => this.#disable(e.target, Object.keys(chkbxRange.getSelectedIds()), true)
			);

			document.addEventListener('click', (e) => {
				if (e.target.classList.contains('js-edit')) {
					this.#edit({correlationid: e.target.dataset.correlationid});
				}
				else if (e.target.classList.contains('js-enable')) {
					this.#enable(e.target, [e.target.dataset.correlationid]);
				}
				else if (e.target.classList.contains('js-disable')) {
					this.#disable(e.target, [e.target.dataset.correlationid]);
				}
			})
		}

		/**
		 * Listens to submit and delete actions from edit form. If gets a successful response, reloads the page.
		 *
		 * @param {object} parameters  Parameters to pass to the edit form.
		 */
		#edit(parameters = {}) {
			const overlay = PopUp('correlation.edit', parameters, {
				dialogueid: 'correlation-form',
				dialogue_class: 'modal-popup-medium',
				prevent_navigation: true
			});

			overlay.$dialogue[0].addEventListener('dialogue.submit', (e) => {
				uncheckTableRows('correlation');
				postMessageOk(e.detail.title);

				if ('messages' in e.detail) {
					postMessageDetails('success', e.detail.messages);
				}

				location.href = location.href;
			});
		}

		/**
		 * Shows confirmation window if multiple records are selected for deletion and sends a POST request to
		 * delete action.
		 *
		 * @param {element} target          The target element that will be passed to post.
		 * @param {array}   correlationids  Event correlation IDs to delete.
		 */
		#delete(target, correlationids) {
			const confirmation = correlationids.length > 1
				? <?= json_encode(_('Delete selected event correlations?')) ?>
				: <?= json_encode(_('Delete selected event correlation?')) ?>;

			if (!window.confirm(confirmation)) {
				return;
			}

			const curl = new Curl('zabbix.php');

			curl.setArgument('action', 'correlation.delete');
			this.#post(target, correlationids, curl);
		}

		/**
		 * Shows confirmation window if multiple records are selected for enabling and sends a POST request to enable
		 * action.
		 *
		 * @param {element} target          The target element that will be passed to post.
		 * @param {array}   correlationids  Event correlation IDs to enable.
		 * @param {bool}    massenable      True if footer button is pressed for mass enable action which brings up
		 *                                  confirmation menu. False if only one element with single link is clicked.
		 */
		#enable(target, correlationids, massenable = false) {
			if (massenable) {
				const confirmation = correlationids.length > 1
					? <?= json_encode(_('Enable selected event correlations?')) ?>
					: <?= json_encode(_('Enable selected event correlation?')) ?>;

				if (!window.confirm(confirmation)) {
					return;
				}
			}

			const curl = new Curl('zabbix.php');

			curl.setArgument('action', 'correlation.enable');
			this.#post(target, correlationids, curl);
		}

		/**
		 * Shows confirmation window if multiple records are selected for disabling and sends a POST request to disable
		 * action.
		 *
		 * @param {element} target          The target element that will be passed to post.
		 * @param {array}   correlationids  Event correlation IDs to disable.
		 * @param {bool}    massdisable     True if footer button is pressed for mass disable action which brings up
		 *                                  confirmation menu. False if only one element with single link is clicked.
		 */
		#disable(target, correlationids, massdisable = false) {
			if (massdisable) {
				const confirmation = correlationids.length > 1
					? <?= json_encode(_('Disable selected event correlations?')) ?>
					: <?= json_encode(_('Disable selected event correlation?')) ?>;

				if (!window.confirm(confirmation)) {
					return;
				}
			}

			const curl = new Curl('zabbix.php');

			curl.setArgument('action', 'correlation.disable');
			this.#post(target, correlationids, curl);
		}

		/**
		 * Sends a POST request to the specified URL with the provided data and handles the response.
		 *
		 * @param {element} target          The target element that will display a loading state during the request.
		 * @param {array}   correlationids  Event correlation IDs to send with the POST request.
		 * @param {object}  url             The URL to send the POST request to.
		 */
		#post(target, correlationids, url) {
			url.setArgument('<?= CCsrfTokenHelper::CSRF_TOKEN_NAME ?>',
				<?= json_encode(CCsrfTokenHelper::get('correlation')) ?>
			);

			target.classList.add('is-loading');

			return fetch(url.getUrl(), {
				method: 'POST',
				headers: {'Content-Type': 'application/json'},
				body: JSON.stringify({correlationids: correlationids})
			})
				.then((response) => response.json())
				.then((response) => {
					if ('error' in response) {
						if ('title' in response.error) {
							postMessageError(response.error.title);
						}

						uncheckTableRows('correlation', response.keepids ?? []);
						postMessageDetails('error', response.error.messages);
					}
					else if ('success' in response) {
						postMessageOk(response.success.title);

						if ('messages' in response.success) {
							postMessageDetails('success', response.success.messages);
						}

						uncheckTableRows('correlation');
					}

					location.href = location.href;
				})
				.catch(() => {
					clearMessages();

					const message_box = makeMessageBox('bad', [<?= json_encode(_('Unexpected server error.')) ?>]);

					addMessage(message_box);
				})
				.finally(() => target.classList.remove('is-loading'));
		}
	};
</script>
